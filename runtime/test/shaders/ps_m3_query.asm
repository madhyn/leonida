; M3 occlusion probe: one color export, with all framebuffer writes disabled by the PSO.
; Same root constants as ps_m3.asm; no depth export, discard or memory side effects.
; Vulkan permits occlusion counts multiplied by the number of written color outputs, even with PRECISE.
; Keep the probe single-output so its count can be compared exactly with the CPU's visible rectangle area.
; fssm-at: 0x10
; fssm-data: 0x33515350 0x20535020
; Console root layout (D3D-448): root arguments from s8, a descriptor table is one dword (its 32-bit VA; the heaps
; live below 4 GiB, so the high half is 0). The first lines rebuild the registers the program below was written for.
s_mov_b32 s5, s13
s_mov_b32 s6, s14
s_mov_b32 s7, s15
v_mov_b32_e32 v2, s5
v_mov_b32_e32 v3, s6
v_mov_b32_e32 v4, s7
v_mov_b32_e32 v5, 1.0
exp mrt0 v2, v3, v4, v5 done vm
s_endpgm
