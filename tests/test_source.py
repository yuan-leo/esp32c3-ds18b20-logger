"""Host-side sanity tests; actual firmware compilation occurs in PlatformIO."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / 'src/main.cpp').read_text()
HTML = (ROOT / 'src/dashboard.h').read_text()
PART = (ROOT / 'partitions.csv').read_text()

class SourceTests(unittest.TestCase):
    def test_sample_interval_and_pullup_documented(self):
        self.assertIn('SAMPLE_INTERVAL_MS = 5000', MAIN)
        self.assertIn('DS18B20_PIN = 4', MAIN)
        self.assertIn('4.7k', MAIN)

    def test_record_and_segments(self):
        self.assertIn('static_assert(sizeof(Sample) == 8', MAIN)
        self.assertIn('SEGMENT_COUNT = 12', MAIN)
        self.assertIn('SEGMENT_BYTES = 96UL * 1024UL', MAIN)
        self.assertIn('WRITE_BATCH = 12', MAIN)

    def test_api_routes_and_no_cdn(self):
        for endpoint in ['/api/status', '/api/history', '/export.csv', '/api/wifi', '/api/time', '/api/clear']:
            self.assertIn(endpoint, MAIN)
        self.assertNotIn('https://', HTML)
        self.assertIn('<canvas id="graph"', HTML)
        self.assertIn('CSV', HTML)

    def test_partition_layout(self):
        self.assertIn('0x10000,  0x200000', PART)
        self.assertIn('0x210000, 0x1f0000', PART)
        self.assertEqual(0x210000+0x1f0000, 0x400000)

    def test_script_matches_offsets(self):
        script=(ROOT/'scripts/make_factory.py').read_text()
        for address in ('0x0', '0x8000', '0x10000'):
            self.assertIn(address, script)

if __name__=='__main__':
    unittest.main()
