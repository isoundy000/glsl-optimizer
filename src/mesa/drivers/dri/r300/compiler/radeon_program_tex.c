/*
 * Copyright (C) 2010 Corbin Simpson
 *
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "radeon_program_tex.h"

#include "../r300_reg.h"

/* Series of transformations to be done on textures. */

static struct rc_src_register shadow_ambient(struct radeon_compiler * c, int tmu)
{
	struct rc_src_register reg = { 0, };

	reg.File = RC_FILE_CONSTANT;
	reg.Index = rc_constants_add_state(&c->Program.Constants, RC_STATE_SHADOW_AMBIENT, tmu);
	reg.Swizzle = RC_SWIZZLE_WWWW;
	return reg;
}

/**
 * Transform TEX, TXP, TXB, and KIL instructions in the following ways:
 *  - implement texture compare (shadow extensions)
 *  - extract non-native source / destination operands
 *  - premultiply texture coordinates for RECT
 *  - extract operand swizzles
 *  - introduce a temporary register when write masks are needed
 */
int radeonTransformTEX(
	struct radeon_compiler * c,
	struct rc_instruction * inst,
	void* data)
{
	struct r300_fragment_program_compiler *compiler =
		(struct r300_fragment_program_compiler*)data;

	if (inst->U.I.Opcode != RC_OPCODE_TEX &&
	    inst->U.I.Opcode != RC_OPCODE_TXB &&
	    inst->U.I.Opcode != RC_OPCODE_TXP &&
	    inst->U.I.Opcode != RC_OPCODE_KIL)
		return 0;

	/* ARB_shadow & EXT_shadow_funcs */
	if (inst->U.I.Opcode != RC_OPCODE_KIL &&
	    c->Program.ShadowSamplers & (1 << inst->U.I.TexSrcUnit)) {
		rc_compare_func comparefunc = compiler->state.unit[inst->U.I.TexSrcUnit].texture_compare_func;

		if (comparefunc == RC_COMPARE_FUNC_NEVER || comparefunc == RC_COMPARE_FUNC_ALWAYS) {
			inst->U.I.Opcode = RC_OPCODE_MOV;

			if (comparefunc == RC_COMPARE_FUNC_ALWAYS) {
				inst->U.I.SrcReg[0].File = RC_FILE_NONE;
				inst->U.I.SrcReg[0].Swizzle = RC_SWIZZLE_1111;
			} else {
				inst->U.I.SrcReg[0] = shadow_ambient(c, inst->U.I.TexSrcUnit);
			}

			return 1;
		} else {
			rc_compare_func comparefunc = compiler->state.unit[inst->U.I.TexSrcUnit].texture_compare_func;
			unsigned int depthmode = compiler->state.unit[inst->U.I.TexSrcUnit].depth_texture_mode;
			struct rc_instruction * inst_rcp = rc_insert_new_instruction(c, inst);
			struct rc_instruction * inst_mad = rc_insert_new_instruction(c, inst_rcp);
			struct rc_instruction * inst_cmp = rc_insert_new_instruction(c, inst_mad);
			int pass, fail;

			inst_rcp->U.I.Opcode = RC_OPCODE_RCP;
			inst_rcp->U.I.DstReg.File = RC_FILE_TEMPORARY;
			inst_rcp->U.I.DstReg.Index = rc_find_free_temporary(c);
			inst_rcp->U.I.DstReg.WriteMask = RC_MASK_W;
			inst_rcp->U.I.SrcReg[0] = inst->U.I.SrcReg[0];
			inst_rcp->U.I.SrcReg[0].Swizzle = RC_SWIZZLE_WWWW;

			inst_cmp->U.I.DstReg = inst->U.I.DstReg;
			inst->U.I.DstReg.File = RC_FILE_TEMPORARY;
			inst->U.I.DstReg.Index = rc_find_free_temporary(c);
			inst->U.I.DstReg.WriteMask = RC_MASK_XYZW;

			inst_mad->U.I.Opcode = RC_OPCODE_MAD;
			inst_mad->U.I.DstReg.File = RC_FILE_TEMPORARY;
			inst_mad->U.I.DstReg.Index = rc_find_free_temporary(c);
			inst_mad->U.I.SrcReg[0] = inst->U.I.SrcReg[0];
			inst_mad->U.I.SrcReg[0].Swizzle = RC_SWIZZLE_ZZZZ;
			inst_mad->U.I.SrcReg[1].File = RC_FILE_TEMPORARY;
			inst_mad->U.I.SrcReg[1].Index = inst_rcp->U.I.DstReg.Index;
			inst_mad->U.I.SrcReg[1].Swizzle = RC_SWIZZLE_WWWW;
			inst_mad->U.I.SrcReg[2].File = RC_FILE_TEMPORARY;
			inst_mad->U.I.SrcReg[2].Index = inst->U.I.DstReg.Index;
			if (depthmode == 0) /* GL_LUMINANCE */
				inst_mad->U.I.SrcReg[2].Swizzle = RC_MAKE_SWIZZLE(RC_SWIZZLE_X, RC_SWIZZLE_Y, RC_SWIZZLE_Z, RC_SWIZZLE_Z);
			else if (depthmode == 2) /* GL_ALPHA */
				inst_mad->U.I.SrcReg[2].Swizzle = RC_SWIZZLE_WWWW;

			/* Recall that SrcReg[0] is tex, SrcReg[2] is r and:
			 *   r  < tex  <=>      -tex+r < 0
			 *   r >= tex  <=> not (-tex+r < 0 */
			if (comparefunc == RC_COMPARE_FUNC_LESS || comparefunc == RC_COMPARE_FUNC_GEQUAL)
				inst_mad->U.I.SrcReg[2].Negate = inst_mad->U.I.SrcReg[2].Negate ^ RC_MASK_XYZW;
			else
				inst_mad->U.I.SrcReg[0].Negate = inst_mad->U.I.SrcReg[0].Negate ^ RC_MASK_XYZW;

			inst_cmp->U.I.Opcode = RC_OPCODE_CMP;
			/* DstReg has been filled out above */
			inst_cmp->U.I.SrcReg[0].File = RC_FILE_TEMPORARY;
			inst_cmp->U.I.SrcReg[0].Index = inst_mad->U.I.DstReg.Index;

			if (comparefunc == RC_COMPARE_FUNC_LESS || comparefunc == RC_COMPARE_FUNC_GREATER) {
				pass = 1;
				fail = 2;
			} else {
				pass = 2;
				fail = 1;
			}

			inst_cmp->U.I.SrcReg[pass].File = RC_FILE_NONE;
			inst_cmp->U.I.SrcReg[pass].Swizzle = RC_SWIZZLE_1111;
			inst_cmp->U.I.SrcReg[fail] = shadow_ambient(c, inst->U.I.TexSrcUnit);
		}
	}

	/* Texture wrap modes don't work on NPOT textures or texrects.
	 *
	 * The game plan is simple. We have two flags, fake_npot and
	 * non_normalized_coords, as well as a tex target. The RECT tex target
	 * will make the emitted code use non-scaled texcoords.
	 *
	 * Non-wrapped/clamped texcoords with NPOT are free in HW. Repeat and
	 * mirroring are not. If we need to repeat, we do:
	 *
	 * MUL temp, texcoord, <scaling factor constant>
	 * FRC temp, temp ; Discard integer portion of coords
	 *
	 * This gives us coords in [0, 1].
	 *
	 * Mirroring is trickier. We're going to start out like repeat:
	 *
	 * MUL temp0, texcoord, <scaling factor constant> ; De-mirror across axes
	 * MUL temp0, abs(temp0), 0.5 ; Pattern repeats in [0, 2]
	 *                            ; so scale to [0, 1]
	 * FRC temp0, temp0 ; Make the pattern repeat
	 * SGE temp1, temp0, 0.5 ; Select components that need to be "reflected"
	 *                       ; across the mirror
	 * MAD temp0, neg(0.5), temp1, temp0 ; Add -0.5 to the
	 *                                   ; selected components
	 * ADD temp0, temp0, temp0 ; Poor man's 2x to undo earlier MUL
	 *
	 * This gives us coords in [0, 1].
	 *
	 * ~ C.
	 */
	if (inst->U.I.Opcode != RC_OPCODE_KIL &&
		(inst->U.I.TexSrcTarget == RC_TEXTURE_RECT ||
			compiler->state.unit[inst->U.I.TexSrcUnit].fake_npot ||
			compiler->state.unit[inst->U.I.TexSrcUnit].non_normalized_coords)) {
		rc_wrap_mode wrapmode = compiler->state.unit[inst->U.I.TexSrcUnit].wrap_mode;
		struct rc_instruction *inst_rect = NULL;
		unsigned temp = rc_find_free_temporary(c);

		if (inst->U.I.TexSrcTarget == RC_TEXTURE_RECT ||
			compiler->state.unit[inst->U.I.TexSrcUnit].non_normalized_coords) {
			inst_rect = rc_insert_new_instruction(c, inst->Prev);

			inst_rect->U.I.Opcode = RC_OPCODE_MUL;
			inst_rect->U.I.DstReg.File = RC_FILE_TEMPORARY;
			inst_rect->U.I.DstReg.Index = temp;
			inst_rect->U.I.SrcReg[0] = inst->U.I.SrcReg[0];
			inst_rect->U.I.SrcReg[1].File = RC_FILE_CONSTANT;
			inst_rect->U.I.SrcReg[1].Index =
				rc_constants_add_state(&c->Program.Constants,
					RC_STATE_R300_TEXRECT_FACTOR, inst->U.I.TexSrcUnit);

			reset_srcreg(&inst->U.I.SrcReg[0]);
			inst->U.I.SrcReg[0].File = RC_FILE_TEMPORARY;
			inst->U.I.SrcReg[0].Index = temp;

			inst->U.I.TexSrcTarget = RC_TEXTURE_2D;
		}

		if (compiler->state.unit[inst->U.I.TexSrcUnit].fake_npot &&
			wrapmode != RC_WRAP_NONE) {
			if (wrapmode == RC_WRAP_REPEAT) {
				/* Both instructions will be paired up. */
				struct rc_instruction *inst_frc = rc_insert_new_instruction(c, inst->Prev);
				struct rc_instruction *inst_mov = rc_insert_new_instruction(c, inst_frc);

				inst_frc->U.I.Opcode = RC_OPCODE_FRC;
				inst_frc->U.I.DstReg.File = RC_FILE_TEMPORARY;
				inst_frc->U.I.DstReg.Index = temp;
				inst_frc->U.I.DstReg.WriteMask = RC_MASK_XYZ;
				inst_frc->U.I.SrcReg[0] = inst->U.I.SrcReg[0];

				/* Preserve W for TXP. */
				inst_mov->U.I.Opcode = RC_OPCODE_MOV;
				inst_mov->U.I.DstReg.File = RC_FILE_TEMPORARY;
				inst_mov->U.I.DstReg.Index = temp;
				inst_mov->U.I.DstReg.WriteMask = RC_MASK_W;
				inst_mov->U.I.SrcReg[0] = inst->U.I.SrcReg[0];

				reset_srcreg(&inst->U.I.SrcReg[0]);
				inst->U.I.SrcReg[0].File = RC_FILE_TEMPORARY;
				inst->U.I.SrcReg[0].Index = temp;
			} else if (wrapmode == RC_WRAP_MIRROR) {
				unsigned temp1;
				/*
				 * MUL temp0, abs(temp0), 0.5
				 * FRC temp0, temp0
				 * SGE temp1, temp0, 0.5
				 * MAD temp0, neg(0.5), temp1, temp0
				 * ADD temp0, temp0, temp0
				 */

				inst_rect = rc_insert_new_instruction(c, inst->Prev);

				inst_rect->U.I.Opcode = RC_OPCODE_MUL;
				inst_rect->U.I.DstReg.File = RC_FILE_TEMPORARY;
				inst_rect->U.I.DstReg.Index = temp;
				inst_rect->U.I.SrcReg[0] = inst->U.I.SrcReg[0];
				inst_rect->U.I.SrcReg[1].Swizzle = RC_MAKE_SWIZZLE_SMEAR(RC_SWIZZLE_HALF);

				inst_rect = rc_insert_new_instruction(c, inst->Prev);

				inst_rect->U.I.Opcode = RC_OPCODE_FRC;
				inst_rect->U.I.DstReg.File = RC_FILE_TEMPORARY;
				inst_rect->U.I.DstReg.Index = temp;
				inst_rect->U.I.SrcReg[0].File = RC_FILE_TEMPORARY;
				inst_rect->U.I.SrcReg[0].Index = temp;

				temp1 = rc_find_free_temporary(c);
				inst_rect = rc_insert_new_instruction(c, inst->Prev);

				inst_rect->U.I.Opcode = RC_OPCODE_SGE;
				inst_rect->U.I.DstReg.File = RC_FILE_TEMPORARY;
				inst_rect->U.I.DstReg.Index = temp1;
				inst_rect->U.I.SrcReg[0].File = RC_FILE_TEMPORARY;
				inst_rect->U.I.SrcReg[0].Index = temp;
				inst_rect->U.I.SrcReg[1].Swizzle = RC_MAKE_SWIZZLE_SMEAR(RC_SWIZZLE_HALF);

				inst_rect = rc_insert_new_instruction(c, inst->Prev);

				inst_rect->U.I.Opcode = RC_OPCODE_MAD;
				inst_rect->U.I.DstReg.File = RC_FILE_TEMPORARY;
				inst_rect->U.I.DstReg.Index = temp;
				inst_rect->U.I.SrcReg[0].File = RC_FILE_TEMPORARY;
				inst_rect->U.I.SrcReg[0].Index = temp1;
				inst_rect->U.I.SrcReg[1].Swizzle = RC_MAKE_SWIZZLE_SMEAR(RC_SWIZZLE_HALF);
				inst_rect->U.I.SrcReg[1].Negate = 1;
				inst_rect->U.I.SrcReg[2].File = RC_FILE_TEMPORARY;
				inst_rect->U.I.SrcReg[2].Index = temp;

				inst_rect = rc_insert_new_instruction(c, inst->Prev);

				inst_rect->U.I.Opcode = RC_OPCODE_ADD;
				inst_rect->U.I.DstReg.File = RC_FILE_TEMPORARY;
				inst_rect->U.I.DstReg.Index = temp;
				inst_rect->U.I.SrcReg[0].File = RC_FILE_TEMPORARY;
				inst_rect->U.I.SrcReg[0].Index = temp;
				inst_rect->U.I.SrcReg[1].File = RC_FILE_TEMPORARY;
				inst_rect->U.I.SrcReg[1].Index = temp;

				reset_srcreg(&inst->U.I.SrcReg[0]);
				inst->U.I.SrcReg[0].File = RC_FILE_TEMPORARY;
				inst->U.I.SrcReg[0].Index = temp;
			}
		}
	}

	/* Cannot write texture to output registers or with masks */
	if (inst->U.I.Opcode != RC_OPCODE_KIL &&
	    (inst->U.I.DstReg.File != RC_FILE_TEMPORARY || inst->U.I.DstReg.WriteMask != RC_MASK_XYZW)) {
		struct rc_instruction * inst_mov = rc_insert_new_instruction(c, inst);

		inst_mov->U.I.Opcode = RC_OPCODE_MOV;
		inst_mov->U.I.DstReg = inst->U.I.DstReg;
		inst_mov->U.I.SrcReg[0].File = RC_FILE_TEMPORARY;
		inst_mov->U.I.SrcReg[0].Index = rc_find_free_temporary(c);

		inst->U.I.DstReg.File = RC_FILE_TEMPORARY;
		inst->U.I.DstReg.Index = inst_mov->U.I.SrcReg[0].Index;
		inst->U.I.DstReg.WriteMask = RC_MASK_XYZW;
	}


	/* Cannot read texture coordinate from constants file */
	if (inst->U.I.SrcReg[0].File != RC_FILE_TEMPORARY && inst->U.I.SrcReg[0].File != RC_FILE_INPUT) {
		struct rc_instruction * inst_mov = rc_insert_new_instruction(c, inst->Prev);

		inst_mov->U.I.Opcode = RC_OPCODE_MOV;
		inst_mov->U.I.DstReg.File = RC_FILE_TEMPORARY;
		inst_mov->U.I.DstReg.Index = rc_find_free_temporary(c);
		inst_mov->U.I.SrcReg[0] = inst->U.I.SrcReg[0];

		reset_srcreg(&inst->U.I.SrcReg[0]);
		inst->U.I.SrcReg[0].File = RC_FILE_TEMPORARY;
		inst->U.I.SrcReg[0].Index = inst_mov->U.I.DstReg.Index;
	}

	return 1;
}