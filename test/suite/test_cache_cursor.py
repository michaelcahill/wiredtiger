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


if __name__ == '__main__':
    import sys
    sys.exit(wttest.main())
