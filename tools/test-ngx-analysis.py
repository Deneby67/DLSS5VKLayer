#!/usr/bin/env python3
"""Reject incomplete, mismatched and failed NGX evidence using synthetic records."""
import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('analysis', Path(__file__).with_name('analyze-ngx-capture.py'))
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


def fixture():
    rows = [
        dict(event='attach_wait'),
        dict(event='create', provider=0, handle=999, handle_generation=1, feature=1, result=1),
        dict(event='capture_begin', request=77, max_samples=1),
        dict(event='evaluate_before', request=77, call=1, provider=0, handle=999,
             handle_generation=1, feature=1, mode='observe_only', before={
                 'resources': {'Depth': {'result': 1, 'resource': {
                     'image': 999, 'view': 888, 'status': 'image_descriptor', 'format': 130,
                     'extent': [1280, 720]}}},
                 'values': {'Reset': {'result': 1, 'value': 0}},
                 'optional_matrices': {'InvViewProjectionMatrix': {'result': 0xbad00010}}}),
        dict(event='evaluate_after', request=77, call=1, result=1),
        dict(event='capture_end', request=77, reason='sample_limit')]
    return [dict(schema=1, pid=200, **row) for row in rows]


class Tests(unittest.TestCase):
    def test_success_and_redaction(self):
        report = analysis.summarize(fixture())
        self.assertEqual(report['successful_evaluations'], 1)
        self.assertFalse(report['fg_ready'])
        descriptor = report['resources']['Depth'][0]
        self.assertNotIn('image', descriptor)
        self.assertNotIn('view', descriptor)
        self.assertNotIn('pid', report)

    def test_incomplete(self):
        with self.assertRaises(ValueError): analysis.summarize(fixture()[:-1])

    def test_reject_bad_pairings(self):
        for field, value in [('pid', 201), ('request', 78), ('call', 2)]:
            rows = fixture()
            rows[4][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError): analysis.summarize(rows)

    def test_failed_evaluation(self):
        rows = fixture(); rows[4]['result'] = 0xbad00005
        with self.assertRaises(ValueError): analysis.summarize(rows)

    def test_unknown_or_reused_feature(self):
        for generation in (0, 2):
            rows = fixture(); rows[3]['handle_generation'] = generation
            with self.subTest(generation=generation), self.assertRaises(ValueError): analysis.summarize(rows)

    def test_duplicate_result(self):
        rows = fixture(); rows.insert(5, copy.deepcopy(rows[4]))
        with self.assertRaises(ValueError): analysis.summarize(rows)


if __name__ == '__main__': unittest.main()
