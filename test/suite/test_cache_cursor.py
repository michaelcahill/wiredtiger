#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#
import wiredtiger, wttest
from wiredtiger import stat
from wtdataset import SimpleDataSet

# Test cache: cursor type
class test_cache_cursor(wttest.WiredTigerTestCase):
    """
    Test the cache: cursor type for iterating over files and pages in cache.
    """
    test_name = __qualname__

    conn_config = 'statistics=(all)'

    def test_cache_file_summary(self):
        """
        Test opening a cache: cursor (file summary mode).
        """
        # Build an object and force some writes.
        ds = SimpleDataSet(self, f'table:{self.test_name}', 100, config='leaf_page_max=4K')
        ds.populate()
        self.session.checkpoint(None)

        # Open cache cursor (file summary mode).
        cache_cur = self.session.open_cursor('cache:')

        found_files = []
        for row in cache_cur:
            # Value format is "qqqq": iteration yields the key followed by the four
            # value fields <dirty, bytes in cache, dirty bytes, update bytes>.
            key, dirty, bytes_in_cache, dirty_bytes, update_bytes = row
            self.assertTrue(key.startswith('cache:file:'), f'Key should start with "cache:file:", got: {key}')
            self.assertIn(dirty, (0, 1), f'dirty should be 0 or 1, got: {dirty}')
            self.assertGreaterEqual(bytes_in_cache, 0)
            self.assertGreaterEqual(dirty_bytes, 0)
            self.assertGreaterEqual(update_bytes, 0)
            self.assertLessEqual(dirty_bytes, bytes_in_cache)
            found_files.append(key)

        # We should have found at least the table file we created.
        self.assertGreater(len(found_files), 0, 'Should find at least one file in cache')

        cache_cur.close()

    def test_cache_file_pages(self):
        """
        Test opening a cache:file:<filename> cursor (page-by-page mode).
        """
        # Build an object and force some writes.
        ds = SimpleDataSet(self, f'table:{self.test_name}_pages', 100, config='leaf_page_max=4K')
        ds.populate()
        self.session.checkpoint(None)

        # Open cache file cursor (page-by-page mode).
        cache_cur = self.session.open_cursor(f'cache:file:{self.test_name}_pages.wt')

        page_count = 0
        for row in cache_cur:
            # Value format is "qqqqqqq": iteration yields the key followed by the seven
            # value fields <dirty, bytes in memory, dirty bytes, update bytes, read gen,
            # page type, pin reason>.
            key, dirty, bytes_in_memory, dirty_bytes, update_bytes, read_gen, page_type, \
                pin_reason = row
            self.assertIsInstance(key, str)
            self.assertTrue(key.startswith('page:'), f'Key should start with "page:", got: {key}')
            self.assertIn(dirty, (0, 1), f'dirty should be 0 or 1, got: {dirty}')
            self.assertGreaterEqual(bytes_in_memory, 0)
            self.assertGreaterEqual(dirty_bytes, 0)
            self.assertGreaterEqual(update_bytes, 0)
            self.assertLessEqual(dirty_bytes, bytes_in_memory)
            self.assertGreaterEqual(read_gen, 0)
            # Page type is one of the WT_PAGE_* values (WT_PAGE_TYPE_COUNT bounds them).
            self.assertGreaterEqual(page_type, 0)
            self.assertLess(page_type, 8)
            # Pin reason is a WT_PAGEPIN_* value; WT_PAGEPIN_NONE means the page can be evicted.
            self.assertGreaterEqual(pin_reason, 0)
            page_count += 1

        # We should have found at least the root page.
        self.assertGreater(page_count, 0, 'Should find at least one page')

        cache_cur.close()


class test_cache_cursor_sample(wttest.WiredTigerTestCase):
    """
    Test sampling pages of a single file with a cache:file:<filename> cursor configured
    with next_random_sample_size.
    """
    test_name = __qualname__

    conn_config = 'statistics=(all)'

    nrows = 20000

    def load(self, uri, key_format='S'):
        """
        Build a file of small leaf pages. Reopen so the tree is read back as reconciled pages
        rather than the large in-memory pages the load leaves behind.
        """
        ds = SimpleDataSet(self, uri, self.nrows, key_format=key_format, config='leaf_page_max=4K')
        ds.populate()
        self.session.checkpoint(None)
        self.reopen_conn()
        return ds

    def warm(self, uri):
        """
        Read the whole file into cache through a table cursor. Warming through the cache cursor
        itself does not work: it reads with WT_READ_WONT_NEED, and those pages are evicted again
        before they can be sampled.
        """
        cur = self.session.open_cursor(uri, None, None)
        for k, v in cur:
            pass
        cur.close()

    def page_keys(self, file_name, config=''):
        cur = self.session.open_cursor(f'cache:file:{file_name}', None, config)
        keys = [row[0] for row in cur]
        cur.close()
        return keys

    def test_sample_size_caps_rows(self):
        """
        A sample size is a hard cap on rows, and a cursor without one still sees the whole file.
        """
        ds = self.load(f'table:{self.test_name}_cap')
        name = f'{self.test_name}_cap.wt'

        self.warm(ds.uri)
        rows = self.page_keys(name, 'next_random_sample_size=10')
        # The stride comes from an estimate of residency, so the row count is approximate and can
        # land a little short of the sample size. The cap is not approximate.
        self.assertTrue(5 <= len(rows) <= 10, f'got {len(rows)} rows for a sample of 10')

        self.warm(ds.uri)
        full = len(self.page_keys(name))
        self.assertGreater(full, 10)
        self.assertLess(len(rows), full)

    def test_sample_size_monotonic(self):
        """
        More samples means at least as many rows, and never more than a full walk.
        """
        ds = self.load(f'table:{self.test_name}_mono')
        name = f'{self.test_name}_mono.wt'

        counts = []
        for n in (5, 20):
            self.warm(ds.uri)
            counts.append(len(self.page_keys(name, f'next_random_sample_size={n}')))
        self.warm(ds.uri)
        full = len(self.page_keys(name))

        self.assertTrue(counts[0] <= counts[1] <= full,
            f'expected rows(5) {counts[0]} <= rows(20) {counts[1]} <= full {full}')

    def test_sample_no_reads(self):
        """
        A sampled walk visits resident pages only, so it never reads the file.
        """
        ds = self.load(f'table:{self.test_name}_reads')
        name = f'{self.test_name}_reads.wt'

        # Warm first: a statistics cursor walks each file to gather its statistics, and on a cold
        # cache that read is charged against the delta being measured here.
        self.warm(ds.uri)
        before = self.get_stat(stat.conn.cache_read)
        rows = self.page_keys(name, 'next_random_sample_size=10')
        after = self.get_stat(stat.conn.cache_read)

        self.assertTrue(1 <= len(rows) <= 10, f'got {len(rows)} rows for a sample of 10')
        self.assertEqual(after - before, 0, 'A sampled walk should read nothing into cache')

    def test_sample_cold_file(self):
        """
        A sampled walk on a connection that has cached nothing stays inside the cap, and the file
        is sampled normally once its pages are in cache.
        """
        ds = self.load(f'table:{self.test_name}_cold')
        name = f'{self.test_name}_cold.wt'

        # No statistics here: gathering them walks every file, which would warm this one.
        rows = self.page_keys(name, 'next_random_sample_size=10')
        self.assertLessEqual(len(rows), 10)

        # The file is there, and sampled normally once its pages are in cache.
        self.warm(ds.uri)
        rows = self.page_keys(name, 'next_random_sample_size=10')
        self.assertTrue(5 <= len(rows) <= 10, f'got {len(rows)} rows for a sample of 10')

    def test_sample_seed_reproducible(self):
        """
        A seeded sample over an unchanged, resident tree is the same sample every time.
        """
        ds = self.load(f'table:{self.test_name}_seed')
        name = f'{self.test_name}_seed.wt'
        seed = 42
        config = f'next_random_sample_size=20,next_random_seed={seed}'

        self.warm(ds.uri)
        first = sorted(self.page_keys(name, config))
        self.warm(ds.uri)
        second = sorted(self.page_keys(name, config))

        self.assertTrue(0 < len(first) <= 20, f'seed {seed} returned {len(first)} rows')
        self.assertEqual(first, second, f'seed {seed} did not reproduce the same sample')

    def test_sample_column_store(self):
        """
        Sampling works on a column-store file.
        """
        ds = self.load(f'table:{self.test_name}_col', key_format='r')
        name = f'{self.test_name}_col.wt'

        self.warm(ds.uri)
        rows = self.page_keys(name, 'next_random_sample_size=10')

        self.assertGreater(len(rows), 0)
        self.assertLessEqual(len(rows), 10)

    def test_sample_reset(self):
        """
        Resetting a sampled cursor starts a fresh pass, capped again.
        """
        ds = self.load(f'table:{self.test_name}_reset')
        name = f'{self.test_name}_reset.wt'

        self.warm(ds.uri)
        cache_cur = self.session.open_cursor(
            f'cache:file:{name}', None, 'next_random_sample_size=10')

        first = 0
        while cache_cur.next() == 0:
            first += 1
        cache_cur.reset()
        second = 0
        while cache_cur.next() == 0:
            second += 1
        cache_cur.close()

        self.assertTrue(5 <= first <= 10, f'first pass returned {first} rows')
        self.assertTrue(0 < second <= 10, f'second pass returned {second} rows')

    def test_sample_rejects_next_random(self):
        """
        Random record semantics do not apply to a cache cursor.
        """
        self.load(f'table:{self.test_name}_nrand')
        name = f'{self.test_name}_nrand.wt'

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(f'cache:file:{name}', None, 'next_random=true'),
            '/not supported by cache cursors/')


if __name__ == '__main__':
    import sys
    sys.exit(wttest.main())
