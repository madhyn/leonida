"""image_msaa_load lowering for dim:SQ_RSRC_IMG_2D_MSAA and dim:SQ_RSRC_IMG_2D_MSAA_ARRAY.

Standard library only (spirv-as and spirv-val are used when they are on PATH); no GPU, no game files.
"""
import os
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

import support

DESC = "s[8:15]"
PRO = [
    "v_mov_b32 v4, 1",
    "v_mov_b32 v5, 2",
    "v_mov_b32 v6, 3",
    "v_mov_b32 v7, 4",
]


def translate(name, line):
    return support.translate_lines(
        name,
        PRO + [line, "exp mrt0 v9, v9, v9, v9 done vm", "s_endpgm"],
    )


class ImageMsaaLoadTests(unittest.TestCase):
    def test_msaa_dims_declare_bindings_20_to_25_and_sample_fetch(self):
        cases = [
            ("2D_MSAA", "v[4:6]", 0, 20, "%v2int"),
            ("2D_MSAA_ARRAY", "v[4:7]", 1, 21, "%v3int"),
        ]
        for dim, addr, arr, base_binding, coord_ty in cases:
            with self.subTest(dim=dim):
                spv = translate(
                    f"ps_msaa_{dim.lower()}",
                    f"image_msaa_load v[9:12], {addr}, {DESC} dmask:0xf dim:SQ_RSRC_IMG_{dim}",
                )
                for cls, st in enumerate(("%float", "%uint", "%int")):
                    self.assertIn(
                        f"%itximg_ms{cls}_{arr} = OpTypeImage {st} 2D 0 {arr} 1 1 Unknown",
                        spv,
                    )
                    self.assertIn(
                        f"OpDecorate %itx_ms{cls}_{arr} Binding {base_binding + 2 * cls}",
                        spv,
                    )
                    self.assertRegex(
                        spv,
                        rf"OpAccessChain %_ptr_UC_itx_ms{cls}_{arr} %itx_ms{cls}_{arr} ",
                    )
                self.assertIn(f"OpCompositeConstruct {coord_ty}", spv)
                self.assertRegex(spv, r"= OpImageFetch %v4float %[^ ]+ %[^ ]+ Sample %[^ ]+")
                self.assertRegex(spv, r"= OpImageFetch %v4uint_st %[^ ]+ %[^ ]+ Sample %[^ ]+")
                self.assertRegex(spv, r"= OpImageFetch %v4int_st %[^ ]+ %[^ ]+ Sample %[^ ]+")
                # dmask:0xf fetches 4 sequential samples (sample + 0..3) and packs their X components.
                self.assertRegex(spv, r"= OpCompositeConstruct %v4float %[^ ]+ %[^ ]+ %[^ ]+ %[^ ]+")

    def test_single_sample_dmask_emits_single_fetch(self):
        spv = translate(
            "ps_msaa_single",
            f"image_msaa_load v9, v[4:6], {DESC} dmask:0x1 dim:SQ_RSRC_IMG_2D_MSAA",
        )
        self.assertEqual(spv.count("= OpImageFetch %v4float "), 1)
        self.assertNotIn("OpIAdd %int", spv)

    def test_a16_and_d16_and_wave64_emulate(self):
        spv = translate(
            "ps_msaa_a16_d16",
            f"image_msaa_load v[9:10], v[4:5], {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D_MSAA a16 d16",
        )
        self.assertRegex(spv, r"OpBitwiseAnd %uint %[^ ]+ %cffff")
        self.assertRegex(spv, r"OpShiftRightLogical %uint %[^ ]+ %c10")
        self.assertIn("OpImageFetch %v4float", spv)
        self.assertIn("OpImageFetch %v4uint_st", spv)

        with mock.patch.object(support.rdna2spirv, "WAVE64_EMULATE", True):
            spv_w64 = support.translate_lines(
                "ps_msaa_w64",
                PRO + [
                    "s_mov_b64 exec, -1",
                    f"image_msaa_load v[9:12], v[4:6], {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D_MSAA",
                    "exp mrt0 v9, v9, v9, v9 done vm",
                    "s_endpgm",
                ],
            )
        self.assertIn("OpImageFetch %v4float", spv_w64)

    def test_unsupported_forms_raise(self):
        bad = [
            f"image_msaa_load v[9:12], v[4:5], {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D",
            f"image_msaa_load v[9:12], v[4:6], {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D_MSAA glc",
            f"image_msaa_load v[9:12], v[4:6], {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D_MSAA tfe",
            f"image_msaa_load v[9:12], v[4:6], {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D_MSAA lwe",
            f"image_msaa_load off, v[4:6], {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D_MSAA",
            f"image_msaa_load v[9:12], v[4:5], {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D_MSAA",
        ]
        for line in bad:
            with self.subTest(line=line):
                with self.assertRaises(support.rdna2spirv.Unsupported):
                    translate("ps_msaa_bad", line)

    @unittest.skipUnless(shutil.which("spirv-as") and shutil.which("spirv-val"),
                         "needs spirv-as and spirv-val on PATH")
    def test_modules_validate(self):
        cases = [
            ("2dms", f"image_msaa_load v[9:12], v[4:6], {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D_MSAA"),
            ("2dms_arr", f"image_msaa_load v[9:12], v[4:7], {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D_MSAA_ARRAY"),
            ("2dms_a16_d16", f"image_msaa_load v[9:10], v[4:5], {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D_MSAA a16 d16"),
            ("2dms_arr_a16", f"image_msaa_load v[9:12], v[4:5], {DESC} dmask:0xf dim:SQ_RSRC_IMG_2D_MSAA_ARRAY a16"),
        ]
        for name, line in cases:
            with self.subTest(name=name):
                spv = translate(f"ps_msaa_val_{name}", line)
                with tempfile.TemporaryDirectory() as tmp:
                    a, b = os.path.join(tmp, "m.spvasm"), os.path.join(tmp, "m.spv")
                    with open(a, "w", encoding="utf-8") as f:
                        f.write(spv)
                    subprocess.run(["spirv-as", "--target-env", "vulkan1.3", "-o", b, a], check=True)
                    subprocess.run(["spirv-val", "--target-env", "vulkan1.3", b], check=True)


if __name__ == "__main__":
    unittest.main()
