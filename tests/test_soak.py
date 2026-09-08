"""Fast observer contracts; no sockets, processes, sleeps or product fixture."""
import unittest
from unittest.mock import patch
from types import SimpleNamespace

import soak


class ObserverTests(unittest.TestCase):
    def test_descriptor_arrows_are_not_tcp_connections(self):
        outputs = ['8192 2.5', 'USER PID\nx 123\nx 123\nx 123\n',
                   'p123\nf0\ntPIPE\nn->0xabc\nf1\ntPIPE\nn->0xdef\n'
                   'f2\ntREG\nn/dev/null\nf3\ntIPv4\nPTCP\nn127.0.0.1:1->127.0.0.1:2\n']
        with patch('soak.platform.system', return_value='Darwin'), patch('soak.subprocess.run') as run:
            run.side_effect = [SimpleNamespace(returncode=0, stdout=s) for s in outputs]
            value = soak.measure(123)
        self.assertEqual(value, dict(rss_kib=8192, ps_cpu_percent=2.5, threads=3,
                                    descriptors=4, tcp_connections=1))

    def test_warmed_rss_trend_distinguishes_plateau_and_growth(self):
        def series(increment):
            return [dict(elapsed_seconds=i*60, processes={'client':dict(rss_kib=8192+i*increment,
                    threads=3, descriptors=7, tcp_connections=1, ps_cpu_percent=0.1)}) for i in range(60)]
        flat = soak.summarize(series(0))['client']
        growing = soak.summarize(series(512))['client']
        self.assertEqual(flat['rss_window_growth_kib'], 0)
        self.assertEqual(flat['rss_slope_kib_per_minute'], 0)
        self.assertGreater(growing['rss_window_growth_kib'], soak.RSS_GROWTH_KIB)
        self.assertGreater(growing['rss_slope_kib_per_minute'], soak.RSS_SLOPE_KIB_PER_MINUTE)

    def test_reconciled_payload_eviction_preserves_duplicate_detection(self):
        peer = soak.Peer('test')
        first = {'payload': {'counter': 2}}
        peer.record_message(first)
        peer.messages.pop(2)
        peer.record_message({'payload': {'counter': 1}})  # Concurrent delivery may be out of order.
        with self.assertRaisesRegex(AssertionError, 'Duplicate'):
            peer.record_message(first)
        self.assertEqual(peer.received, 2)
        self.assertEqual(len(peer.messages), 1)


if __name__ == '__main__':
    unittest.main()
