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
# [TEST_TAGS]
# cache_log
# [END_TAGS]
#

import json
import os
import re
import time
import wiredtiger, wttest
from wtdataset import SimpleDataSet

# Helper: find the WiredTigerCache log file written to a directory.
def find_cache_log_file(dir):
    """Return the path of the WiredTigerCache log file in dir, or None."""
    for f in os.listdir(dir):
        if f.startswith('WiredTigerCache'):
            return os.path.join(dir, f)
    return None


# Test basic cache_log configuration and operation.
class test_cache_log_basic(wttest.WiredTigerTestCase):
    """
    Test the cache_log configuration: enabling/disabling, path, wait interval,
    and basic log file creation.
    """
    test_name = __qualname__

    def test_cache_log_no_server(self):
        """
        Test that setting wait=0 does not create a cache log file.
        """
        ds = SimpleDataSet(self, f'table:{self.test_name}', 100)
        ds.populate()
        self.session.checkpoint(None)

        # Connect with cache_log disabled (wait=0 is the default).
        self.reopen_conn()

        # No cache log file should exist.
        self.assertIsNone(find_cache_log_file('.'),
                          'No cache log file expected when wait=0')

    def test_cache_log_enabled_creates_file(self):
        """
        Test that enabling cache_log with wait>0 creates a log file.
        """
        ds = SimpleDataSet(self, f'table:{self.test_name}', 100)
        ds.populate()
        self.session.checkpoint(None)

        # Reconfigure to enable cache logging with a short wait.
        self.conn.reconfigure(f'cache_log=(wait=1)')

        # Wait for at least one log write.
        self.wait_for_log_file(5)

        # Reconfigure back to disable.
        self.conn.reconfigure('cache_log=(wait=0)')

        # The log file should exist now.
        log_file = find_cache_log_file('.')
        self.assertIsNotNone(log_file, 'Cache log file should exist after enabling')

    def test_cache_log_custom_path(self):
        """
        Test that the cache_log.path option places the log file in the
        specified directory.
        """
        os.mkdir('logdir')
        try:
            ds = SimpleDataSet(self, f'table:{self.test_name}', 100)
            ds.populate()
            self.session.checkpoint(None)

            self.conn.reconfigure('cache_log=(path=logdir,wait=1)')

            # Wait for log file to appear in logdir.
            self.wait_for_log_file_in('logdir', 5)

            # No log file in the home directory.
            self.assertIsNone(find_cache_log_file('.'))

            # Reconfigure back to disable.
            self.conn.reconfigure('cache_log=(wait=0)')
        finally:
            import shutil
            shutil.rmtree('logdir', ignore_errors=True)

    def test_cache_log_reconfigure_disable(self):
        """
        Test reconfiguring from enabled to disabled stops the server.
        """
        ds = SimpleDataSet(self, f'table:{self.test_name}', 100)
        ds.populate()
        self.session.checkpoint(None)

        self.conn.reconfigure('cache_log=(wait=1)')
        self.wait_for_log_file(5)

        # Disable the server.
        self.conn.reconfigure('cache_log=(wait=0)')

        # Wait a bit and confirm no further writes.
        time.sleep(3)
        log_file = find_cache_log_file('.')
        self.assertIsNotNone(log_file)
        mtime_before = os.path.getmtime(log_file)
        time.sleep(2)
        mtime_after = os.path.getmtime(log_file)
        self.assertEqual(mtime_before, mtime_after,
                         'Log file should not be modified after disabling server')

    def wait_for_log_file(self, timeout=10):
        """Wait until a WiredTigerCache file appears in the home directory."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if find_cache_log_file('.') is not None:
                return
            time.sleep(0.5)
        self.fail('Timed out waiting for cache log file')

    def wait_for_log_file_in(self, subdir, timeout=10):
        """Wait until a WiredTigerCache file appears in the given subdirectory."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if find_cache_log_file(subdir) is not None:
                return
            time.sleep(0.5)
        self.fail(f'Timed out waiting for cache log file in {subdir}')


# Test cache_log timestamp formatting.
class test_cache_log_timestamp(wttest.WiredTigerTestCase):
    """
    Test the cache_log timestamp configuration option.
    """
    test_name = __qualname__

    def test_cache_log_custom_timestamp(self):
        """
        Test that a custom timestamp format is used in log output.
        """
        ds = SimpleDataSet(self, f'table:{self.test_name}', 100)
        ds.populate()
        self.session.checkpoint(None)

        # Use a simple, distinctive timestamp format.
        self.conn.reconfigure('cache_log=(wait=1,timestamp="%H:%M:%S")')
        self.wait_for_log_file(5)
        self.conn.reconfigure('cache_log=(wait=0)')

        log_file = find_cache_log_file('.')
        self.assertIsNotNone(log_file)

        with open(log_file, 'r') as f:
            content = f.read()

        # Each non-blank line should start with the custom timestamp.
        for line in content.strip().split('\n'):
            line = line.strip()
            if not line:
                continue
            # The timestamp format is HH:MM:SS.
            self.assertRegex(line, r'^\d{2}:\d{2}:\d{2}',
                             f'Line should start with custom timestamp: {line}')

    def test_cache_log_default_timestamp(self):
        """
        Test that the default timestamp format is used when none is specified.
        """
        ds = SimpleDataSet(self, f'table:{self.test_name}', 100)
        ds.populate()
        self.session.checkpoint(None)

        self.conn.reconfigure('cache_log=(wait=1)')
        self.wait_for_log_file(5)
        self.conn.reconfigure('cache_log=(wait=0)')

        log_file = find_cache_log_file('.')
        self.assertIsNotNone(log_file)

        with open(log_file, 'r') as f:
            content = f.read()

        # The default format is "%b %d %H:%M:%S" (e.g., "Jan 01 12:00:00").
        for line in content.strip().split('\n'):
            line = line.strip()
            if not line:
                continue
            self.assertRegex(line, r'^[A-Z][a-z]{2}\s+\d{2}\s+\d{2}:\d{2}:\d{2}',
                             f'Line should match default timestamp format: {line}')

    def wait_for_log_file(self, timeout=10):
        """Wait until a WiredTigerCache file appears in the home directory."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if find_cache_log_file('.') is not None:
                return
            time.sleep(0.5)
        self.fail('Timed out waiting for cache log file')


# Test cache_log JSON format.
class test_cache_log_json(wttest.WiredTigerTestCase):
    """
    Test the cache_log json configuration option.
    """
    test_name = __qualname__

    def test_cache_log_json_format(self):
        """
        Test that json=true produces valid JSON output.
        """
        ds = SimpleDataSet(self, f'table:{self.test_name}', 100)
        ds.populate()
        self.session.checkpoint(None)

        self.conn.reconfigure('cache_log=(wait=1,json=true)')
        self.wait_for_log_file(5)
        self.conn.reconfigure('cache_log=(wait=0)')

        log_file = find_cache_log_file('.')
        self.assertIsNotNone(log_file)

        with open(log_file, 'r') as f:
            content = f.read()

        # The output should be valid JSON (possibly with trailing newline).
        import json
        try:
            data = json.loads(content)
        except json.JSONDecodeError as e:
            self.fail(f'Cache log output is not valid JSON: {e}\nContent:\n{content}')

        # JSON output should have 'version' and 'localTime' fields.
        self.assertIn('version', data, 'JSON should contain "version" field')
        self.assertIn('localTime', data, 'JSON should contain "localTime" field')
        self.assertIn('wiredTigerCache', data,
                      'JSON should contain "wiredTigerCache" field')

    def test_cache_log_json_default_timestamp(self):
        """
        Test that when json=true, the default timestamp format is the
        MongoDB-compatible ISO 8601 format.
        """
        ds = SimpleDataSet(self, f'table:{self.test_name}', 100)
        ds.populate()
        self.session.checkpoint(None)

        self.conn.reconfigure('cache_log=(wait=1,json=true)')
        self.wait_for_log_file(5)
        self.conn.reconfigure('cache_log=(wait=0)')

        log_file = find_cache_log_file('.')
        self.assertIsNotNone(log_file)

        with open(log_file, 'r') as f:
            content = f.read()

        import json
        data = json.loads(content)
        local_time = data.get('localTime', '')

        # The JSON default timestamp format is "%Y-%m-%dT%H:%M:%S.000Z".
        self.assertRegex(local_time,
                         r'^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$',
                         f'JSON default timestamp should be ISO 8601: {local_time}')

    def wait_for_log_file(self, timeout=10):
        """Wait until a WiredTigerCache file appears in the home directory."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if find_cache_log_file('.') is not None:
                return
            time.sleep(0.5)
        self.fail('Timed out waiting for cache log file')


# Test cache_log on_close behavior.
class test_cache_log_on_close(wttest.WiredTigerTestCase):
    """
    Test the cache_log on_close configuration option.
    """
    test_name = __qualname__

    def test_cache_log_on_close_logs_at_close(self):
        """
        Test that on_close=true logs cache state when the connection closes.
        """
        ds = SimpleDataSet(self, f'table:{self.test_name}', 100)
        ds.populate()
        self.session.checkpoint(None)

        # Configure on_close but do NOT enable the server (wait=0).
        self.reopen_conn_with(f'cache_log=(on_close=true)')

        # Close the connection.
        self.conn.close()

        # A log file should have been created.
        log_file = find_cache_log_file('.')
        self.assertIsNotNone(log_file,
                             'Cache log file should exist after close with on_close=true')

        with open(log_file, 'r') as f:
            content = f.read()
        self.assertTrue(len(content) > 0,
                        'Cache log file should not be empty after on_close')

    def test_cache_log_on_close_no_log_without_flag(self):
        """
        Test that on_close=false does not log at close.
        """
        ds = SimpleDataSet(self, f'table:{self.test_name}', 100)
        ds.populate()
        self.session.checkpoint(None)

        # Remove any prior log files.
        import glob
        for f in glob.glob('WiredTigerCache*'):
            os.remove(f)

        self.reopen_conn_with('cache_log=(on_close=false)')
        self.conn.close()

        log_file = find_cache_log_file('.')
        self.assertIsNone(log_file,
                          'No cache log file expected when on_close=false')

    def test_cache_log_on_close_with_running_server(self):
        """
        Test that on_close logs when the server is running.
        The __cachelog_on_close function checks for a running server and
        returns EINVAL, but the error is swallowed during connection close
        so the close still succeeds.
        """
        ds = SimpleDataSet(self, f'table:{self.test_name}', 100)
        ds.populate()
        self.session.checkpoint(None)

        # Enable the server with on_close also set.
        self.conn.reconfigure('cache_log=(wait=1,on_close=true)')

        # Give the server a moment to start and write at least one log.
        time.sleep(2)

        # Close should succeed even though on_close cannot log with the
        # server running (the internal error is swallowed during close).
        self.conn.close()

    def reopen_conn_with(self, config):
        """Close and reopen the connection with the given config."""
        self.conn.close()
        self.conn = self.wiredtiger_open('.', config)
        self.session = self.conn.open_session(None)


# Test cache_log with multiple files and data.
class test_cache_log_content(wttest.WiredTigerTestCase):
    """
    Test that the cache log contains meaningful cache state data.
    """
    test_name = __qualname__

    def test_cache_log_contains_file_entries(self):
        """
        Test that the cache log contains entries for files in cache.
        """
        ds = SimpleDataSet(self, f'table:{self.test_name}', 100)
        ds.populate()
        self.session.checkpoint(None)

        # Also create a second table to ensure multiple entries.
        ds2 = SimpleDataSet(self, f'table:{self.test_name}_2', 50)
        ds2.populate()
        self.session.checkpoint(None)

        self.conn.reconfigure('cache_log=(wait=1)')
        self.wait_for_log_file(5)
        self.conn.reconfigure('cache_log=(wait=0)')

        log_file = find_cache_log_file('.')
        self.assertIsNotNone(log_file)

        with open(log_file, 'r') as f:
            content = f.read()

        # In non-JSON mode, each line has format: <timestamp> <dirty> <bytes> <dirty_bytes> <update_bytes> <name>
        # We should see entries for both tables.
        self.assertIn(self.test_name, content,
                      'Log should contain the first table name')
        self.assertIn(f'{self.test_name}_2', content,
                      'Log should contain the second table name')

    def test_cache_log_json_contains_file_entries(self):
        """
        Test that JSON-format cache log contains entries for files in cache.
        """
        ds = SimpleDataSet(self, f'table:{self.test_name}', 100)
        ds.populate()
        self.session.checkpoint(None)

        ds2 = SimpleDataSet(self, f'table:{self.test_name}_2', 50)
        ds2.populate()
        self.session.checkpoint(None)

        self.conn.reconfigure('cache_log=(wait=1,json=true)')
        self.wait_for_log_file(5)
        self.conn.reconfigure('cache_log=(wait=0)')

        log_file = find_cache_log_file('.')
        self.assertIsNotNone(log_file)

        with open(log_file, 'r') as f:
            content = f.read()

        import json
        data = json.loads(content)
        cache_data = data['wiredTigerCache']

        # The wiredTigerCache section should be a dict mapping file names to state.
        self.assertIsInstance(cache_data, dict,
                              'wiredTigerCache should be a dict')
        self.assertGreater(len(cache_data), 0,
                           'Should have at least one file entry')

    def test_cache_log_values_reasonable(self):
        """
        Test that the numeric values in the cache log are reasonable.
        """
        ds = SimpleDataSet(self, f'table:{self.test_name}', 100)
        ds.populate()
        self.session.checkpoint(None)

        self.conn.reconfigure('cache_log=(wait=1)')
        self.wait_for_log_file(5)
        self.conn.reconfigure('cache_log=(wait=0)')

        log_file = find_cache_log_file('.')
        self.assertIsNotNone(log_file)

        with open(log_file, 'r') as f:
            lines = f.readlines()

        # Filter out blank lines.
        lines = [l.strip() for l in lines if l.strip()]
        self.assertGreater(len(lines), 0, 'Should have at least one log line')

        for line in lines:
            parts = line.split()
            # Format: <timestamp> <dirty> <bytes_in_cache> <dirty_bytes> <update_bytes> <name>
            # Timestamp may be multi-token (e.g., "Jan 01 12:00:00").
            # We need at least 6 parts (3 for timestamp + 4 numeric + name).
            self.assertGreaterEqual(len(parts), 6,
                                    f'Line should have at least 6 parts: {line}')
            # The numeric fields should be parseable.
            try:
                dirty = int(parts[3])
                bytes_in_cache = int(parts[4])
                dirty_bytes = int(parts[5])
                update_bytes = int(parts[6])
            except ValueError:
                self.fail(f'Numeric fields should be parseable: {line}')

            self.assertIn(dirty, (0, 1), f'dirty should be 0 or 1, got: {dirty}')
            self.assertGreaterEqual(bytes_in_cache, 0)
            self.assertGreaterEqual(dirty_bytes, 0)
            self.assertGreaterEqual(update_bytes, 0)

    def wait_for_log_file(self, timeout=10):
        """Wait until a WiredTigerCache file appears in the home directory."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if find_cache_log_file('.') is not None:
                return
            time.sleep(0.5)
        self.fail('Timed out waiting for cache log file')


# Test cache_log page sampling.
class test_cache_log_sample(wttest.WiredTigerTestCase):
    """
    Test the cache_log sample configuration option: a per-file dump returns at most that many
    pages, and sample=0 logs every page.
    """
    test_name = __qualname__

    def setUp(self):
        super().setUp()
        # Populate enough pages that a small sample is a strict subset of the file's resident
        # pages; the data fits comfortably in the cache, so residency is not a factor.
        ds = SimpleDataSet(self, f'table:{self.test_name}', 100000)
        ds.populate()
        self.session.checkpoint(None)

    def _log_pages_for_table(self):
        log_file = find_cache_log_file('.')
        self.assertIsNotNone(log_file)

        with open(log_file, 'r') as f:
            data = json.loads(f.read())
        cache_data = data['wiredTigerCache']

        keys = [k for k in cache_data if self.test_name in k]
        self.assertEqual(len(keys), 1,
                         f'Expected exactly one entry for the table, got: {list(cache_data)}')
        return cache_data[keys[0]]

    def test_cache_log_sample_limits_pages(self):
        """
        Test that sample=N limits a per-file dump to at most N pages.
        """
        sample = 10
        self.conn.reconfigure(f'cache_log=(wait=1,json=true,sample={sample})')
        self.wait_for_log_file(5)
        self.conn.reconfigure('cache_log=(wait=0)')

        pages = self._log_pages_for_table()
        self.assertGreater(len(pages), 0, 'Sampled dump should return some pages')
        self.assertLessEqual(len(pages), sample,
                             f'Sampled dump should return at most {sample} pages')

    def test_cache_log_sample_zero_logs_all(self):
        """
        Test that sample=0 logs every resident page, not just a sample.
        """
        self.conn.reconfigure('cache_log=(wait=1,json=true,sample=0)')
        self.wait_for_log_file(5)
        self.conn.reconfigure('cache_log=(wait=0)')

        pages = self._log_pages_for_table()
        self.assertGreater(len(pages), 10,
                           'An unsampled dump should return far more than a small sample')

    def test_cache_log_default_sample(self):
        """
        Test that the default sample of 10000 pages leaves a small file unsampled.
        """
        self.conn.reconfigure('cache_log=(wait=1,json=true)')
        self.wait_for_log_file(5)
        self.conn.reconfigure('cache_log=(wait=0)')

        pages = self._log_pages_for_table()
        self.assertGreater(len(pages), 10,
                           'A file with few pages should be fully logged under the default sample')

    def wait_for_log_file(self, timeout=10):
        """Wait until a WiredTigerCache file appears in the home directory."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if find_cache_log_file('.') is not None:
                return
            time.sleep(0.5)
        self.fail('Timed out waiting for cache log file')


if __name__ == '__main__':
    import sys
    sys.exit(wttest.main())
