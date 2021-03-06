/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "effects/GrCustomXfermode.h"

#include "GrCoordTransform.h"
#include "GrContext.h"
#include "GrFragmentProcessor.h"
#include "GrInvariantOutput.h"
#include "GrPipeline.h"
#include "GrProcessor.h"
#include "GrShaderCaps.h"
#include "GrTexture.h"
#include "glsl/GrGLSLBlend.h"
#include "glsl/GrGLSLFragmentProcessor.h"
#include "glsl/GrGLSLFragmentShaderBuilder.h"
#include "glsl/GrGLSLProgramDataManager.h"
#include "glsl/GrGLSLUniformHandler.h"
#include "glsl/GrGLSLXferProcessor.h"

bool GrCustomXfermode::IsSupportedMode(SkBlendMode mode) {
    return (int)mode  > (int)SkBlendMode::kLastCoeffMode &&
           (int)mode <= (int)SkBlendMode::kLastMode;
}

///////////////////////////////////////////////////////////////////////////////
// Static helpers
///////////////////////////////////////////////////////////////////////////////

static GrBlendEquation hw_blend_equation(SkBlendMode mode) {
    enum { kOffset = kOverlay_GrBlendEquation - (int)SkBlendMode::kOverlay };
    return static_cast<GrBlendEquation>((int)mode + kOffset);

    GR_STATIC_ASSERT(kOverlay_GrBlendEquation == (int)SkBlendMode::kOverlay + kOffset);
    GR_STATIC_ASSERT(kDarken_GrBlendEquation == (int)SkBlendMode::kDarken + kOffset);
    GR_STATIC_ASSERT(kLighten_GrBlendEquation == (int)SkBlendMode::kLighten + kOffset);
    GR_STATIC_ASSERT(kColorDodge_GrBlendEquation == (int)SkBlendMode::kColorDodge + kOffset);
    GR_STATIC_ASSERT(kColorBurn_GrBlendEquation == (int)SkBlendMode::kColorBurn + kOffset);
    GR_STATIC_ASSERT(kHardLight_GrBlendEquation == (int)SkBlendMode::kHardLight + kOffset);
    GR_STATIC_ASSERT(kSoftLight_GrBlendEquation == (int)SkBlendMode::kSoftLight + kOffset);
    GR_STATIC_ASSERT(kDifference_GrBlendEquation == (int)SkBlendMode::kDifference + kOffset);
    GR_STATIC_ASSERT(kExclusion_GrBlendEquation == (int)SkBlendMode::kExclusion + kOffset);
    GR_STATIC_ASSERT(kMultiply_GrBlendEquation == (int)SkBlendMode::kMultiply + kOffset);
    GR_STATIC_ASSERT(kHSLHue_GrBlendEquation == (int)SkBlendMode::kHue + kOffset);
    GR_STATIC_ASSERT(kHSLSaturation_GrBlendEquation == (int)SkBlendMode::kSaturation + kOffset);
    GR_STATIC_ASSERT(kHSLColor_GrBlendEquation == (int)SkBlendMode::kColor + kOffset);
    GR_STATIC_ASSERT(kHSLLuminosity_GrBlendEquation == (int)SkBlendMode::kLuminosity + kOffset);
    GR_STATIC_ASSERT(kGrBlendEquationCnt == (int)SkBlendMode::kLastMode + 1 + kOffset);
}

static bool can_use_hw_blend_equation(GrBlendEquation equation,
                                      const GrPipelineAnalysis& analysis,
                                      const GrCaps& caps) {
    if (!caps.advancedBlendEquationSupport()) {
        return false;
    }
    if (analysis.fUsesPLSDstRead) {
        return false;
    }
    if (analysis.fCoveragePOI.isFourChannelOutput()) {
        return false; // LCD coverage must be applied after the blend equation.
    }
    if (caps.canUseAdvancedBlendEquation(equation)) {
        return false;
    }
    return true;
}

///////////////////////////////////////////////////////////////////////////////
// Xfer Processor
///////////////////////////////////////////////////////////////////////////////

class CustomXP : public GrXferProcessor {
public:
    CustomXP(SkBlendMode mode, GrBlendEquation hwBlendEquation)
        : fMode(mode),
          fHWBlendEquation(hwBlendEquation) {
        this->initClassID<CustomXP>();
    }

    CustomXP(const DstTexture* dstTexture, bool hasMixedSamples, SkBlendMode mode)
        : INHERITED(dstTexture, true, hasMixedSamples),
          fMode(mode),
          fHWBlendEquation(static_cast<GrBlendEquation>(-1)) {
        this->initClassID<CustomXP>();
    }

    const char* name() const override { return "Custom Xfermode"; }

    GrGLSLXferProcessor* createGLSLInstance() const override;

    SkBlendMode mode() const { return fMode; }
    bool hasHWBlendEquation() const { return -1 != static_cast<int>(fHWBlendEquation); }

    GrBlendEquation hwBlendEquation() const {
        SkASSERT(this->hasHWBlendEquation());
        return fHWBlendEquation;
    }

private:
    GrXferProcessor::OptFlags onGetOptimizations(const GrPipelineAnalysis&,
                                                 bool doesStencilWrite,
                                                 GrColor* overrideColor,
                                                 const GrCaps& caps) const override;

    void onGetGLSLProcessorKey(const GrShaderCaps& caps, GrProcessorKeyBuilder* b) const override;

    GrXferBarrierType onXferBarrier(const GrRenderTarget*, const GrCaps&) const override;

    void onGetBlendInfo(BlendInfo*) const override;

    bool onIsEqual(const GrXferProcessor& xpBase) const override;

    const SkBlendMode      fMode;
    const GrBlendEquation  fHWBlendEquation;

    typedef GrXferProcessor INHERITED;
};

///////////////////////////////////////////////////////////////////////////////

class GLCustomXP : public GrGLSLXferProcessor {
public:
    GLCustomXP(const GrXferProcessor&) {}
    ~GLCustomXP() override {}

    static void GenKey(const GrXferProcessor& p, const GrShaderCaps& caps,
                       GrProcessorKeyBuilder* b) {
        const CustomXP& xp = p.cast<CustomXP>();
        uint32_t key = 0;
        if (xp.hasHWBlendEquation()) {
            SkASSERT(caps.advBlendEqInteraction() > 0);  // 0 will mean !xp.hasHWBlendEquation().
            key |= caps.advBlendEqInteraction();
            GR_STATIC_ASSERT(GrShaderCaps::kLast_AdvBlendEqInteraction < 4);
        }
        if (!xp.hasHWBlendEquation() || caps.mustEnableSpecificAdvBlendEqs()) {
            key |= (int)xp.mode() << 3;
        }
        b->add32(key);
    }

private:
    void emitOutputsForBlendState(const EmitArgs& args) override {
        const CustomXP& xp = args.fXP.cast<CustomXP>();
        SkASSERT(xp.hasHWBlendEquation());

        GrGLSLXPFragmentBuilder* fragBuilder = args.fXPFragBuilder;
        fragBuilder->enableAdvancedBlendEquationIfNeeded(xp.hwBlendEquation());

        // Apply coverage by multiplying it into the src color before blending. Mixed samples will
        // "just work" automatically. (See onGetOptimizations())
        if (args.fInputCoverage) {
            fragBuilder->codeAppendf("%s = %s * %s;",
                                     args.fOutputPrimary, args.fInputCoverage, args.fInputColor);
        } else {
            fragBuilder->codeAppendf("%s = %s;", args.fOutputPrimary, args.fInputColor);
        }
    }

    void emitBlendCodeForDstRead(GrGLSLXPFragmentBuilder* fragBuilder,
                                 GrGLSLUniformHandler* uniformHandler,
                                 const char* srcColor,
                                 const char* srcCoverage,
                                 const char* dstColor,
                                 const char* outColor,
                                 const char* outColorSecondary,
                                 const GrXferProcessor& proc) override {
        const CustomXP& xp = proc.cast<CustomXP>();
        SkASSERT(!xp.hasHWBlendEquation());

        GrGLSLBlend::AppendMode(fragBuilder, srcColor, dstColor, outColor, xp.mode());

        // Apply coverage.
        INHERITED::DefaultCoverageModulation(fragBuilder, srcCoverage, dstColor, outColor,
                                             outColorSecondary, xp);
    }

    void onSetData(const GrGLSLProgramDataManager&, const GrXferProcessor&) override {}

    typedef GrGLSLXferProcessor INHERITED;
};

///////////////////////////////////////////////////////////////////////////////

void CustomXP::onGetGLSLProcessorKey(const GrShaderCaps& caps, GrProcessorKeyBuilder* b) const {
    GLCustomXP::GenKey(*this, caps, b);
}

GrGLSLXferProcessor* CustomXP::createGLSLInstance() const {
    SkASSERT(this->willReadDstColor() != this->hasHWBlendEquation());
    return new GLCustomXP(*this);
}

bool CustomXP::onIsEqual(const GrXferProcessor& other) const {
    const CustomXP& s = other.cast<CustomXP>();
    return fMode == s.fMode && fHWBlendEquation == s.fHWBlendEquation;
}

GrXferProcessor::OptFlags CustomXP::onGetOptimizations(const GrPipelineAnalysis& analysis,
                                                       bool doesStencilWrite,
                                                       GrColor* overrideColor,
                                                       const GrCaps& caps) const {
    /*
      Most the optimizations we do here are based on tweaking alpha for coverage.

      The general SVG blend equation is defined in the spec as follows:

        Dca' = B(Sc, Dc) * Sa * Da + Y * Sca * (1-Da) + Z * Dca * (1-Sa)
        Da'  = X * Sa * Da + Y * Sa * (1-Da) + Z * Da * (1-Sa)

      (Note that Sca, Dca indicate RGB vectors that are premultiplied by alpha,
       and that B(Sc, Dc) is a mode-specific function that accepts non-multiplied
       RGB colors.)

      For every blend mode supported by this class, i.e. the "advanced" blend
      modes, X=Y=Z=1 and this equation reduces to the PDF blend equation.

      It can be shown that when X=Y=Z=1, these equations can modulate alpha for
      coverage.


      == Color ==

      We substitute Y=Z=1 and define a blend() function that calculates Dca' in
      terms of premultiplied alpha only:

        blend(Sca, Dca, Sa, Da) = {Dca : if Sa == 0,
                                   Sca : if Da == 0,
                                   B(Sca/Sa, Dca/Da) * Sa * Da + Sca * (1-Da) + Dca * (1-Sa) : if
      Sa,Da != 0}

      And for coverage modulation, we use a post blend src-over model:

        Dca'' = f * blend(Sca, Dca, Sa, Da) + (1-f) * Dca

      (Where f is the fractional coverage.)

      Next we show that canTweakAlphaForCoverage() is true by proving the
      following relationship:

        blend(f*Sca, Dca, f*Sa, Da) == f * blend(Sca, Dca, Sa, Da) + (1-f) * Dca

      General case (f,Sa,Da != 0):

        f * blend(Sca, Dca, Sa, Da) + (1-f) * Dca
          = f * (B(Sca/Sa, Dca/Da) * Sa * Da + Sca * (1-Da) + Dca * (1-Sa)) + (1-f) * Dca  [Sa,Da !=
      0, definition of blend()]
          = B(Sca/Sa, Dca/Da) * f*Sa * Da + f*Sca * (1-Da) + f*Dca * (1-Sa) + Dca - f*Dca
          = B(Sca/Sa, Dca/Da) * f*Sa * Da + f*Sca - f*Sca * Da + f*Dca - f*Dca * Sa + Dca - f*Dca
          = B(Sca/Sa, Dca/Da) * f*Sa * Da + f*Sca - f*Sca * Da - f*Dca * Sa + Dca
          = B(Sca/Sa, Dca/Da) * f*Sa * Da + f*Sca * (1-Da) - f*Dca * Sa + Dca
          = B(Sca/Sa, Dca/Da) * f*Sa * Da + f*Sca * (1-Da) + Dca * (1 - f*Sa)
          = B(f*Sca/f*Sa, Dca/Da) * f*Sa * Da + f*Sca * (1-Da) + Dca * (1 - f*Sa)  [f!=0]
          = blend(f*Sca, Dca, f*Sa, Da)  [definition of blend()]

      Corner cases (Sa=0, Da=0, and f=0):

        Sa=0: f * blend(Sca, Dca, Sa, Da) + (1-f) * Dca
                = f * Dca + (1-f) * Dca  [Sa=0, definition of blend()]
                = Dca
                = blend(0, Dca, 0, Da)  [definition of blend()]
                = blend(f*Sca, Dca, f*Sa, Da)  [Sa=0]

        Da=0: f * blend(Sca, Dca, Sa, Da) + (1-f) * Dca
                = f * Sca + (1-f) * Dca  [Da=0, definition of blend()]
                = f * Sca  [Da=0]
                = blend(f*Sca, 0, f*Sa, 0)  [definition of blend()]
                = blend(f*Sca, Dca, f*Sa, Da)  [Da=0]

        f=0: f * blend(Sca, Dca, Sa, Da) + (1-f) * Dca
               = Dca  [f=0]
               = blend(0, Dca, 0, Da)  [definition of blend()]
               = blend(f*Sca, Dca, f*Sa, Da)  [f=0]

      == Alpha ==

      We substitute X=Y=Z=1 and define a blend() function that calculates Da':

        blend(Sa, Da) = Sa * Da + Sa * (1-Da) + Da * (1-Sa)
                      = Sa * Da + Sa - Sa * Da + Da - Da * Sa
                      = Sa + Da - Sa * Da

      We use the same model for coverage modulation as we did with color:

        Da'' = f * blend(Sa, Da) + (1-f) * Da

      And show that canTweakAlphaForCoverage() is true by proving the following
      relationship:

        blend(f*Sa, Da) == f * blend(Sa, Da) + (1-f) * Da


        f * blend(Sa, Da) + (1-f) * Da
          = f * (Sa + Da - Sa * Da) + (1-f) * Da
          = f*Sa + f*Da - f*Sa * Da + Da - f*Da
          = f*Sa - f*Sa * Da + Da
          = f*Sa + Da - f*Sa * Da
          = blend(f*Sa, Da)
     */

    OptFlags flags = kNone_OptFlags;
    if (analysis.fColorPOI.allStagesMultiplyInput()) {
        flags |= kCanTweakAlphaForCoverage_OptFlag;
    }
    if (this->hasHWBlendEquation() && analysis.fCoveragePOI.isSolidWhite()) {
        flags |= kIgnoreCoverage_OptFlag;
    }
    return flags;
}

GrXferBarrierType CustomXP::onXferBarrier(const GrRenderTarget* rt, const GrCaps& caps) const {
    if (this->hasHWBlendEquation() && !caps.advancedCoherentBlendEquationSupport()) {
        return kBlend_GrXferBarrierType;
    }
    return kNone_GrXferBarrierType;
}

void CustomXP::onGetBlendInfo(BlendInfo* blendInfo) const {
    if (this->hasHWBlendEquation()) {
        blendInfo->fEquation = this->hwBlendEquation();
    }
}

///////////////////////////////////////////////////////////////////////////////
class CustomXPFactory : public GrXPFactory {
public:
    CustomXPFactory(SkBlendMode mode);

    void getInvariantBlendedColor(const GrProcOptInfo& colorPOI,
                                  GrXPFactory::InvariantBlendedColor*) const override;

private:
    GrXferProcessor* onCreateXferProcessor(const GrCaps& caps,
                                           const GrPipelineAnalysis&,
                                           bool hasMixedSamples,
                                           const DstTexture*) const override;

    bool onWillReadDstColor(const GrCaps&, const GrPipelineAnalysis&) const override;

    bool onIsEqual(const GrXPFactory& xpfBase) const override {
        const CustomXPFactory& xpf = xpfBase.cast<CustomXPFactory>();
        return fMode == xpf.fMode;
    }

    GR_DECLARE_XP_FACTORY_TEST;

    SkBlendMode     fMode;
    GrBlendEquation fHWBlendEquation;

    typedef GrXPFactory INHERITED;
};

CustomXPFactory::CustomXPFactory(SkBlendMode mode)
    : fMode(mode),
      fHWBlendEquation(hw_blend_equation(mode)) {
    SkASSERT(GrCustomXfermode::IsSupportedMode(fMode));
    this->initClassID<CustomXPFactory>();
}

GrXferProcessor* CustomXPFactory::onCreateXferProcessor(const GrCaps& caps,
                                                        const GrPipelineAnalysis& analysis,
                                                        bool hasMixedSamples,
                                                        const DstTexture* dstTexture) const {
    if (can_use_hw_blend_equation(fHWBlendEquation, analysis, caps)) {
        SkASSERT(!dstTexture || !dstTexture->texture());
        return new CustomXP(fMode, fHWBlendEquation);
    }
    return new CustomXP(dstTexture, hasMixedSamples, fMode);
}

bool CustomXPFactory::onWillReadDstColor(const GrCaps& caps,
                                         const GrPipelineAnalysis& analysis) const {
    return !can_use_hw_blend_equation(fHWBlendEquation, analysis, caps);
}

void CustomXPFactory::getInvariantBlendedColor(const GrProcOptInfo& colorPOI,
                                               InvariantBlendedColor* blendedColor) const {
    blendedColor->fWillBlendWithDst = true;
    blendedColor->fKnownColorFlags = kNone_GrColorComponentFlags;
}

GR_DEFINE_XP_FACTORY_TEST(CustomXPFactory);
sk_sp<GrXPFactory> CustomXPFactory::TestCreate(GrProcessorTestData* d) {
    int mode = d->fRandom->nextRangeU((int)SkBlendMode::kLastCoeffMode + 1,
                                      (int)SkBlendMode::kLastSeparableMode);

    return sk_sp<GrXPFactory>(new CustomXPFactory(static_cast<SkBlendMode>(mode)));
}

///////////////////////////////////////////////////////////////////////////////

sk_sp<GrXPFactory> GrCustomXfermode::MakeXPFactory(SkBlendMode mode) {
    if (!GrCustomXfermode::IsSupportedMode(mode)) {
        return nullptr;
    } else {
        return sk_sp<GrXPFactory>(new CustomXPFactory(mode));
    }
}
