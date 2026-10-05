from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path


TEST_ROOT = Path(__file__).resolve().parent


def load_module(name: str, filename: str):
    spec = importlib.util.spec_from_file_location(name, TEST_ROOT / filename)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class HarnessContractTests(unittest.TestCase):
    def test_personal_plugin_harness_rejects_empty_or_drifted_discovery(self) -> None:
        module = load_module(
            "stage14_personal_plugin_transaction_tests",
            "test_personal_plugin_transaction.py",
        )
        self.assertTrue(hasattr(module, "EXPECTED_TEST_NAMES"))
        self.assertTrue(hasattr(module, "validate_discovered_test_names"))
        expected = set(module.EXPECTED_TEST_NAMES)
        self.assertGreater(len(expected), 0)
        module.validate_discovered_test_names(expected, expected)
        with self.assertRaisesRegex(RuntimeError, "TEST_DISCOVERY_CONTRACT_MISMATCH"):
            module.validate_discovered_test_names(set(), expected)
        with self.assertRaisesRegex(RuntimeError, "TEST_DISCOVERY_CONTRACT_MISMATCH"):
            module.validate_discovered_test_names(expected | {"test_unexpected"}, expected)

    def test_managed_config_harness_rejects_empty_or_drifted_discovery(self) -> None:
        module = load_module(
            "stage14_managed_config_transaction_tests",
            "test_managed_config_transaction.py",
        )
        self.assertTrue(hasattr(module, "EXPECTED_TEST_NAMES"))
        self.assertTrue(hasattr(module, "validate_discovered_test_names"))
        expected = set(module.EXPECTED_TEST_NAMES)
        self.assertGreater(len(expected), 0)
        module.validate_discovered_test_names(expected, expected)
        with self.assertRaisesRegex(RuntimeError, "TEST_DISCOVERY_CONTRACT_MISMATCH"):
            module.validate_discovered_test_names(set(), expected)
        with self.assertRaisesRegex(RuntimeError, "TEST_DISCOVERY_CONTRACT_MISMATCH"):
            module.validate_discovered_test_names(expected - {next(iter(expected))}, expected)


if __name__ == "__main__":
    unittest.main(verbosity=2)
