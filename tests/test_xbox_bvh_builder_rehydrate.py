"""Tests for RT-XDXR-BVH-REHYDRATE-BATCHED2X.

Verifies that runtime/shims/xbox_bvh_builder.cpp implements
XboxBvhBuilder::RehydrateRaytracingAccelerationStructureBatched2X,
validates arguments (null list, null descs, count=0), builds or deserializes
structures via ID3D12GraphicsCommandList4, and registers the mangled export.
Synthetic test only; no retail game data or GPU required.
"""
import re
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parent.parent
XBOX_BVH_CPP = ROOT / "runtime" / "shims" / "xbox_bvh_builder.cpp"
XBOX_BVH_TEST_CPP = ROOT / "runtime" / "test" / "xbox_bvh_builder_test.cpp"


class XboxBvhBuilderRehydrateTest(unittest.TestCase):
    def test_rehydrate_implementation_exists(self):
        self.assertTrue(XBOX_BVH_CPP.is_file(), f"{XBOX_BVH_CPP} must exist")
        content = XBOX_BVH_CPP.read_text(encoding="utf-8")

        # Function definition
        m = re.search(r"HRESULT builder_rehydrate_batched2x\([^)]*\)\s*\{(.*?)\n\}", content, re.DOTALL)
        self.assertIsNotNone(m, "builder_rehydrate_batched2x must be defined in xbox_bvh_builder.cpp")
        body = m.group(1)

        # Must not be a stub returning E_NOTIMPL
        self.assertNotIn("return E_NOTIMPL;", body, "builder_rehydrate_batched2x must not return E_NOTIMPL")

        # Must return S_OK on empty count
        self.assertIn("if (count == 0) return S_OK;", body)

        # Must validate descs and list
        self.assertIn("if (!descs) return E_INVALIDARG;", body)
        self.assertIn("if (!list) return E_INVALIDARG;", body)

        # Must convert to list4 and call Build or Copy
        self.assertIn("ID3D12GraphicsCommandList4* list4 = list4_of(list);", body)
        self.assertIn("BuildRaytracingAccelerationStructure", body)
        self.assertIn("D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE", body)

        # Must release list4 and return S_OK
        self.assertIn("list4->Release();", body)
        self.assertIn("return S_OK;", body)

    def test_mangled_export_registered(self):
        content = XBOX_BVH_CPP.read_text(encoding="utf-8")
        # In C++, string literals split across lines are concatenated by the compiler.
        compact = re.sub(r'"\s*\n\s*"', '', content)
        symbol = "?RehydrateRaytracingAccelerationStructureBatched2X@XboxBvhBuilder@@QEAAJPEAUID3D12GraphicsCommandList8@@IPEBUD3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC@@PEB_K@Z"
        self.assertIn(symbol, compact, f"Symbol {symbol} must be registered in kEntries")
        self.assertIn("&builder_rehydrate_batched2x", content)

    def test_test_harness_validates_rehydrate(self):
        self.assertTrue(XBOX_BVH_TEST_CPP.is_file(), f"{XBOX_BVH_TEST_CPP} must exist")
        test_content = XBOX_BVH_TEST_CPP.read_text(encoding="utf-8")
        self.assertIn("rehydrate(builder, list.p, 1, &rehydrate_desc, &source) == S_OK", test_content)
        self.assertIn("rehydrate(builder, nullptr, 1, &rehydrate_desc, &source) == E_INVALIDARG", test_content)
        self.assertIn("rehydrate(builder, list.p, 0, nullptr, nullptr) == S_OK", test_content)


if __name__ == "__main__":
    unittest.main()
