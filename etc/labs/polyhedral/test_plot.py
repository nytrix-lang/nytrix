#!/usr/bin/env python3
import unittest
import xml.etree.ElementTree as ET
from plot import point_set, conflicts, render, order, tile_group


def statement(extents, triangle=0, accesses=()):
    return {'domain': [{'extent': n, 'symbolic': False} for n in extents],
            'triangle': triangle, 'external': False, 'accesses': list(accesses)}


def access(base, write, coefficients, constant=0):
    return dict(base=base, write=write, width=8, coefficients=coefficients, constant=constant)


def model(*statements, axes=(0, 1), block=0):
    return dict(version=1, key='test<&>', dimensions=len(axes), axes=axes,
                tile=0, inner_tile=0, wavefront=False, skew=1,
                register_block=block, statements=statements)


class PlotTests(unittest.TestCase):
    def test_domains(self):
        for t, n in enumerate((16, 6, 10, 6, 10)):
            self.assertEqual(len(list(point_set(statement([4, 4], t), 4, 0))), n)

    def test_inversion(self):
        # (0,1) writes (1,0)'s read, before interchange.
        s = statement([2, 2], accesses=[access(0, False, [16, 8], -8),
                                        access(0, True, [16, 8])])
        m = model(s, axes=(1, 0))
        p = [list(point_set(s, 2, 0))]
        self.assertIn(((0, (0, 1)), (0, (1, 0)), 'RAW'), conflicts(m, p))
        svg = render(m, 2)
        self.assertIn('1 sampled inversions', svg)
        ET.fromstring(svg)

    def test_fusion(self):
        a = statement([3, 3], accesses=[access(0, True, [24, 8])])
        b = statement([3, 3], accesses=[access(0, False, [24, 8])])
        svg = render(model(a, b), 3)
        self.assertIn('9 displayed conflicts', svg)
        self.assertIn('0 sampled inversions', svg)
        ET.fromstring(svg)

    def test_3d_and_tail(self):
        s = statement([2, 7, 3])
        m = model(s, axes=(0, 1, 2), block=4)
        self.assertEqual(len(list(point_set(s, 7, 2))), 14)
        self.assertNotEqual(tile_group(m, (0, 4, 2), s), tile_group(m, (0, 5, 2), s))
        self.assertLess(order(m, (0, 4, 2), s), order(m, (0, 5, 0), s))
        ET.fromstring(render(m, 7, 2))
        with self.assertRaises(ValueError):
            render(m, 7, 3)


if __name__ == '__main__':
    unittest.main()
