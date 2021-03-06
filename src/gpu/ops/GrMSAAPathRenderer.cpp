/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "GrMSAAPathRenderer.h"

#include "GrAuditTrail.h"
#include "GrClip.h"
#include "GrDefaultGeoProcFactory.h"
#include "GrFixedClip.h"
#include "GrMesh.h"
#include "GrOpFlushState.h"
#include "GrPathStencilSettings.h"
#include "GrPathUtils.h"
#include "GrPipelineBuilder.h"
#include "SkGeometry.h"
#include "SkTraceEvent.h"
#include "gl/GrGLVaryingHandler.h"
#include "glsl/GrGLSLFragmentShaderBuilder.h"
#include "glsl/GrGLSLGeometryProcessor.h"
#include "glsl/GrGLSLProgramDataManager.h"
#include "glsl/GrGLSLUtil.h"
#include "glsl/GrGLSLVertexShaderBuilder.h"
#include "ops/GrMeshDrawOp.h"
#include "ops/GrRectOpFactory.h"

static const float kTolerance = 0.5f;

////////////////////////////////////////////////////////////////////////////////
// Helpers for drawPath

static inline bool single_pass_shape(const GrShape& shape) {
    if (!shape.inverseFilled()) {
        return shape.knownToBeConvex();
    }
    return false;
}

GrPathRenderer::StencilSupport GrMSAAPathRenderer::onGetStencilSupport(const GrShape& shape) const {
    if (single_pass_shape(shape)) {
        return GrPathRenderer::kNoRestriction_StencilSupport;
    } else {
        return GrPathRenderer::kStencilOnly_StencilSupport;
    }
}

struct MSAALineVertices {
    struct Vertex {
        SkPoint fPosition;
        SkColor fColor;
    };
    Vertex* vertices;
    Vertex* nextVertex;
#ifdef SK_DEBUG
    Vertex* verticesEnd;
#endif
    uint16_t* indices;
    uint16_t* nextIndex;
};

struct MSAAQuadVertices {
    struct Vertex {
        SkPoint fPosition;
        SkPoint fUV;
        SkColor fColor;
    };
    Vertex* vertices;
    Vertex* nextVertex;
#ifdef SK_DEBUG
    Vertex* verticesEnd;
#endif
    uint16_t* indices;
    uint16_t* nextIndex;
};

static inline void append_contour_edge_indices(uint16_t fanCenterIdx,
                                               uint16_t edgeV0Idx,
                                               MSAALineVertices& lines) {
    *(lines.nextIndex++) = fanCenterIdx;
    *(lines.nextIndex++) = edgeV0Idx;
    *(lines.nextIndex++) = edgeV0Idx + 1;
}

static inline void add_quad(MSAALineVertices& lines, MSAAQuadVertices& quads, const SkPoint pts[],
                            SkColor color, bool indexed, uint16_t subpathLineIdxStart) {
    SkASSERT(lines.nextVertex < lines.verticesEnd);
    *lines.nextVertex = { pts[2], color };
    if (indexed) {
        int prevIdx = (uint16_t) (lines.nextVertex - lines.vertices - 1);
        if (prevIdx > subpathLineIdxStart) {
            append_contour_edge_indices(subpathLineIdxStart, prevIdx, lines);
        }
    }
    lines.nextVertex++;

    SkASSERT(quads.nextVertex + 2 < quads.verticesEnd);
    // the texture coordinates are drawn from the Loop-Blinn rendering algorithm
    *(quads.nextVertex++) = { pts[0], SkPoint::Make(0.0, 0.0), color };
    *(quads.nextVertex++) = { pts[1], SkPoint::Make(0.5, 0.0), color };
    *(quads.nextVertex++) = { pts[2], SkPoint::Make(1.0, 1.0), color };
    if (indexed) {
        uint16_t offset = (uint16_t) (quads.nextVertex - quads.vertices) - 3;
        *(quads.nextIndex++) = offset++;
        *(quads.nextIndex++) = offset++;
        *(quads.nextIndex++) = offset++;
    }
}

class MSAAQuadProcessor : public GrGeometryProcessor {
public:
    static GrGeometryProcessor* Create(const SkMatrix& viewMatrix) {
        return new MSAAQuadProcessor(viewMatrix);
    }

    virtual ~MSAAQuadProcessor() {}

    const char* name() const override { return "MSAAQuadProcessor"; }

    const Attribute* inPosition() const { return fInPosition; }
    const Attribute* inUV() const { return fInUV; }
    const Attribute* inColor() const { return fInColor; }
    const SkMatrix& viewMatrix() const { return fViewMatrix; }

    class GLSLProcessor : public GrGLSLGeometryProcessor {
    public:
        GLSLProcessor(const GrGeometryProcessor& qpr) {}

        void onEmitCode(EmitArgs& args, GrGPArgs* gpArgs) override {
            const MSAAQuadProcessor& qp = args.fGP.cast<MSAAQuadProcessor>();
            GrGLSLVertexBuilder* vsBuilder = args.fVertBuilder;
            GrGLSLVaryingHandler* varyingHandler = args.fVaryingHandler;
            GrGLSLUniformHandler* uniformHandler = args.fUniformHandler;

            // emit attributes
            varyingHandler->emitAttributes(qp);
            varyingHandler->addPassThroughAttribute(qp.inColor(), args.fOutputColor);

            GrGLSLVertToFrag uv(kVec2f_GrSLType);
            varyingHandler->addVarying("uv", &uv, kHigh_GrSLPrecision);
            vsBuilder->codeAppendf("%s = %s;", uv.vsOut(), qp.inUV()->fName);

            // Setup position
            this->setupPosition(vsBuilder, uniformHandler, gpArgs, qp.inPosition()->fName,
                                qp.viewMatrix(), &fViewMatrixUniform);

            // emit transforms
            this->emitTransforms(vsBuilder, varyingHandler, uniformHandler, gpArgs->fPositionVar,
                                 qp.inPosition()->fName, SkMatrix::I(),
                                 args.fFPCoordTransformHandler);

            GrGLSLPPFragmentBuilder* fsBuilder = args.fFragBuilder;
            fsBuilder->codeAppendf("if (%s.x * %s.x >= %s.y) discard;", uv.fsIn(), uv.fsIn(),
                                                                        uv.fsIn());
            fsBuilder->codeAppendf("%s = vec4(1.0);", args.fOutputCoverage);
        }

        static inline void GenKey(const GrGeometryProcessor& gp,
                                  const GrShaderCaps&,
                                  GrProcessorKeyBuilder* b) {
            const MSAAQuadProcessor& qp = gp.cast<MSAAQuadProcessor>();
            uint32_t key = 0;
            key |= qp.viewMatrix().hasPerspective() ? 0x1 : 0x0;
            key |= qp.viewMatrix().isIdentity() ? 0x2: 0x0;
            b->add32(key);
        }

        void setData(const GrGLSLProgramDataManager& pdman, const GrPrimitiveProcessor& gp,
                     FPCoordTransformIter&& transformIter) override {
            const MSAAQuadProcessor& qp = gp.cast<MSAAQuadProcessor>();
            if (!qp.viewMatrix().isIdentity()) {
                float viewMatrix[3 * 3];
                GrGLSLGetMatrix<3>(viewMatrix, qp.viewMatrix());
                pdman.setMatrix3f(fViewMatrixUniform, viewMatrix);
            }
            this->setTransformDataHelper(SkMatrix::I(), pdman, &transformIter);
        }

    private:
        typedef GrGLSLGeometryProcessor INHERITED;

        UniformHandle fViewMatrixUniform;
    };

    virtual void getGLSLProcessorKey(const GrShaderCaps& caps,
                                   GrProcessorKeyBuilder* b) const override {
        GLSLProcessor::GenKey(*this, caps, b);
    }

    virtual GrGLSLPrimitiveProcessor* createGLSLInstance(const GrShaderCaps&) const override {
        return new GLSLProcessor(*this);
    }

private:
    MSAAQuadProcessor(const SkMatrix& viewMatrix)
        : fViewMatrix(viewMatrix) {
        this->initClassID<MSAAQuadProcessor>();
        fInPosition = &this->addVertexAttrib("inPosition", kVec2f_GrVertexAttribType,
                                             kHigh_GrSLPrecision);
        fInUV = &this->addVertexAttrib("inUV", kVec2f_GrVertexAttribType, kHigh_GrSLPrecision);
        fInColor = &this->addVertexAttrib("inColor", kVec4ub_GrVertexAttribType);
        this->setSampleShading(1.0f);
    }

    const Attribute* fInPosition;
    const Attribute* fInUV;
    const Attribute* fInColor;
    SkMatrix         fViewMatrix;

    GR_DECLARE_GEOMETRY_PROCESSOR_TEST;

    typedef GrGeometryProcessor INHERITED;
};

class MSAAPathOp final : public GrMeshDrawOp {
public:
    DEFINE_OP_CLASS_ID
    static sk_sp<GrDrawOp> Make(GrColor color, const SkPath& path, const SkMatrix& viewMatrix,
                                const SkRect& devBounds) {
        int contourCount;
        int maxLineVertices;
        int maxQuadVertices;
        ComputeWorstCasePointCount(path, &contourCount, &maxLineVertices, &maxQuadVertices);
        bool isIndexed = contourCount > 1;
        if (isIndexed &&
            (maxLineVertices > kMaxIndexedVertexCnt || maxQuadVertices > kMaxIndexedVertexCnt)) {
            return nullptr;
        }

        return sk_sp<GrDrawOp>(new MSAAPathOp(color, path, viewMatrix, devBounds, maxLineVertices,
                                              maxQuadVertices, isIndexed));
    }

    const char* name() const override { return "MSAAPathOp"; }

    SkString dumpInfo() const override {
        SkString string;
        string.appendf("Indexed: %d\n", fIsIndexed);
        for (const auto& path : fPaths) {
            string.appendf("Color: 0x%08x\n", path.fColor);
        }
        string.append(DumpPipelineInfo(*this->pipeline()));
        string.append(INHERITED::dumpInfo());
        return string;
    }

private:
    MSAAPathOp(GrColor color, const SkPath& path, const SkMatrix& viewMatrix,
               const SkRect& devBounds, int maxLineVertices, int maxQuadVertices, bool isIndexed)
            : INHERITED(ClassID())
            , fViewMatrix(viewMatrix)
            , fMaxLineVertices(maxLineVertices)
            , fMaxQuadVertices(maxQuadVertices)
            , fIsIndexed(isIndexed) {
        fPaths.emplace_back(PathInfo{color, path});
        this->setBounds(devBounds, HasAABloat::kNo, IsZeroArea::kNo);
    }

    void getPipelineAnalysisInput(GrPipelineAnalysisDrawOpInput* input) const override {
        input->pipelineColorInput()->setKnownFourComponents(fPaths[0].fColor);
        input->pipelineCoverageInput()->setKnownSingleComponent(0xff);
    }

    void applyPipelineOptimizations(const GrPipelineOptimizations& optimizations) override {
        if (!optimizations.readsColor()) {
            fPaths[0].fColor = GrColor_ILLEGAL;
        }
        optimizations.getOverrideColorIfSet(&fPaths[0].fColor);
    }

    static void ComputeWorstCasePointCount(const SkPath& path, int* subpaths,
                                           int* outLinePointCount, int* outQuadPointCount) {
        int linePointCount = 0;
        int quadPointCount = 0;
        *subpaths = 1;

        bool first = true;

        SkPath::Iter iter(path, true);
        SkPath::Verb verb;

        SkPoint pts[4];
        while ((verb = iter.next(pts)) != SkPath::kDone_Verb) {
            switch (verb) {
                case SkPath::kLine_Verb:
                    linePointCount += 1;
                    break;
                case SkPath::kConic_Verb: {
                    SkScalar weight = iter.conicWeight();
                    SkAutoConicToQuads converter;
                    converter.computeQuads(pts, weight, kTolerance);
                    int quadPts = converter.countQuads();
                    linePointCount += quadPts;
                    quadPointCount += 3 * quadPts;
                }
                case SkPath::kQuad_Verb:
                    linePointCount += 1;
                    quadPointCount += 3;
                    break;
                case SkPath::kCubic_Verb: {
                    SkSTArray<15, SkPoint, true> quadPts;
                    GrPathUtils::convertCubicToQuads(pts, kTolerance, &quadPts);
                    int count = quadPts.count();
                    linePointCount += count / 3;
                    quadPointCount += count;
                    break;
                }
                case SkPath::kMove_Verb:
                    linePointCount += 1;
                    if (!first) {
                        ++(*subpaths);
                    }
                    break;
                default:
                    break;
            }
            first = false;
        }
        *outLinePointCount = linePointCount;
        *outQuadPointCount = quadPointCount;
    }

    void onPrepareDraws(Target* target) const override {
        if (fMaxLineVertices == 0) {
            SkASSERT(fMaxQuadVertices == 0);
            return;
        }

        GrPrimitiveType primitiveType = fIsIndexed ? kTriangles_GrPrimitiveType
                                                   : kTriangleFan_GrPrimitiveType;

        // allocate vertex / index buffers
        const GrBuffer* lineVertexBuffer;
        int firstLineVertex;
        MSAALineVertices lines;
        size_t lineVertexStride = sizeof(MSAALineVertices::Vertex);
        lines.vertices = (MSAALineVertices::Vertex*) target->makeVertexSpace(lineVertexStride,
                                                                             fMaxLineVertices,
                                                                             &lineVertexBuffer,
                                                                             &firstLineVertex);
        if (!lines.vertices) {
            SkDebugf("Could not allocate vertices\n");
            return;
        }
        lines.nextVertex = lines.vertices;
        SkDEBUGCODE(lines.verticesEnd = lines.vertices + fMaxLineVertices;)

        MSAAQuadVertices quads;
        size_t quadVertexStride = sizeof(MSAAQuadVertices::Vertex);
        SkAutoFree quadVertexPtr(sk_malloc_throw(fMaxQuadVertices * quadVertexStride));
        quads.vertices = (MSAAQuadVertices::Vertex*) quadVertexPtr.get();
        quads.nextVertex = quads.vertices;
        SkDEBUGCODE(quads.verticesEnd = quads.vertices + fMaxQuadVertices;)

        const GrBuffer* lineIndexBuffer = nullptr;
        int firstLineIndex;
        if (fIsIndexed) {
            lines.indices =
                    target->makeIndexSpace(3 * fMaxLineVertices, &lineIndexBuffer, &firstLineIndex);
            if (!lines.indices) {
                SkDebugf("Could not allocate indices\n");
                return;
            }
            lines.nextIndex = lines.indices;
        } else {
            lines.indices = nullptr;
            lines.nextIndex = nullptr;
        }

        SkAutoFree quadIndexPtr;
        if (fIsIndexed) {
            quads.indices = (uint16_t*)sk_malloc_throw(3 * fMaxQuadVertices * sizeof(uint16_t));
            quadIndexPtr.set(quads.indices);
            quads.nextIndex = quads.indices;
        } else {
            quads.indices = nullptr;
            quads.nextIndex = nullptr;
        }

        // fill buffers
        for (int i = 0; i < fPaths.count(); i++) {
            const PathInfo& pathInfo = fPaths[i];

            if (!this->createGeom(lines,
                                  quads,
                                  pathInfo.fPath,
                                  fViewMatrix,
                                  pathInfo.fColor,
                                  fIsIndexed)) {
                return;
            }
        }
        int lineVertexOffset = (int) (lines.nextVertex - lines.vertices);
        int lineIndexOffset = (int) (lines.nextIndex - lines.indices);
        SkASSERT(lineVertexOffset <= fMaxLineVertices && lineIndexOffset <= 3 * fMaxLineVertices);
        int quadVertexOffset = (int) (quads.nextVertex - quads.vertices);
        int quadIndexOffset = (int) (quads.nextIndex - quads.indices);
        SkASSERT(quadVertexOffset <= fMaxQuadVertices && quadIndexOffset <= 3 * fMaxQuadVertices);

        if (lineVertexOffset) {
            sk_sp<GrGeometryProcessor> lineGP;
            {
                using namespace GrDefaultGeoProcFactory;
                lineGP = GrDefaultGeoProcFactory::Make(Color(Color::kAttribute_Type),
                                                       Coverage(255),
                                                       LocalCoords(LocalCoords::kUnused_Type),
                                                       fViewMatrix);
            }
            SkASSERT(lineVertexStride == lineGP->getVertexStride());

            GrMesh lineMeshes;
            if (fIsIndexed) {
                lineMeshes.initIndexed(primitiveType, lineVertexBuffer, lineIndexBuffer,
                                         firstLineVertex, firstLineIndex, lineVertexOffset,
                                         lineIndexOffset);
            } else {
                lineMeshes.init(primitiveType, lineVertexBuffer, firstLineVertex,
                                  lineVertexOffset);
            }
            target->draw(lineGP.get(), lineMeshes);
        }

        if (quadVertexOffset) {
            sk_sp<const GrGeometryProcessor> quadGP(MSAAQuadProcessor::Create(fViewMatrix));
            SkASSERT(quadVertexStride == quadGP->getVertexStride());

            const GrBuffer* quadVertexBuffer;
            int firstQuadVertex;
            MSAAQuadVertices::Vertex* quadVertices = (MSAAQuadVertices::Vertex*)
                    target->makeVertexSpace(quadVertexStride, quadVertexOffset, &quadVertexBuffer,
                                            &firstQuadVertex);
            memcpy(quadVertices, quads.vertices, quadVertexStride * quadVertexOffset);
            GrMesh quadMeshes;
            if (fIsIndexed) {
                const GrBuffer* quadIndexBuffer;
                int firstQuadIndex;
                uint16_t* quadIndices = (uint16_t*) target->makeIndexSpace(quadIndexOffset,
                                                                           &quadIndexBuffer,
                                                                           &firstQuadIndex);
                memcpy(quadIndices, quads.indices, sizeof(uint16_t) * quadIndexOffset);
                quadMeshes.initIndexed(kTriangles_GrPrimitiveType, quadVertexBuffer,
                                       quadIndexBuffer, firstQuadVertex, firstQuadIndex,
                                       quadVertexOffset, quadIndexOffset);
            } else {
                quadMeshes.init(kTriangles_GrPrimitiveType, quadVertexBuffer, firstQuadVertex,
                                quadVertexOffset);
            }
            target->draw(quadGP.get(), quadMeshes);
        }
    }

    bool onCombineIfPossible(GrOp* t, const GrCaps& caps) override {
        MSAAPathOp* that = t->cast<MSAAPathOp>();
        if (!GrPipeline::CanCombine(*this->pipeline(), this->bounds(), *that->pipeline(),
                                     that->bounds(), caps)) {
            return false;
        }

        if (!fViewMatrix.cheapEqualTo(that->fViewMatrix)) {
            return false;
        }

        // If we grow to include 2+ paths we will be indexed.
        if (((fMaxLineVertices + that->fMaxLineVertices) > kMaxIndexedVertexCnt) ||
            ((fMaxQuadVertices + that->fMaxQuadVertices) > kMaxIndexedVertexCnt)) {
            return false;
        }

        fPaths.push_back_n(that->fPaths.count(), that->fPaths.begin());
        this->joinBounds(*that);
        fIsIndexed = true;
        fMaxLineVertices += that->fMaxLineVertices;
        fMaxQuadVertices += that->fMaxQuadVertices;
        return true;
    }

    bool createGeom(MSAALineVertices& lines,
                    MSAAQuadVertices& quads,
                    const SkPath& path,
                    const SkMatrix& m,
                    SkColor color,
                    bool isIndexed) const {
        {
            uint16_t subpathIdxStart = (uint16_t) (lines.nextVertex - lines.vertices);

            SkPoint pts[4];

            bool first = true;
            SkPath::Iter iter(path, true);

            bool done = false;
            while (!done) {
                SkPath::Verb verb = iter.next(pts);
                switch (verb) {
                    case SkPath::kMove_Verb:
                        if (!first) {
                            uint16_t currIdx = (uint16_t) (lines.nextVertex - lines.vertices);
                            subpathIdxStart = currIdx;
                        }
                        SkASSERT(lines.nextVertex < lines.verticesEnd);
                        *(lines.nextVertex++) = { pts[0], color };
                        break;
                    case SkPath::kLine_Verb:
                        if (isIndexed) {
                            uint16_t prevIdx = (uint16_t) (lines.nextVertex - lines.vertices - 1);
                            if (prevIdx > subpathIdxStart) {
                                append_contour_edge_indices(subpathIdxStart, prevIdx, lines);
                            }
                        }
                        SkASSERT(lines.nextVertex < lines.verticesEnd);
                        *(lines.nextVertex++) = { pts[1], color };
                        break;
                    case SkPath::kConic_Verb: {
                        SkScalar weight = iter.conicWeight();
                        SkAutoConicToQuads converter;
                        const SkPoint* quadPts = converter.computeQuads(pts, weight, kTolerance);
                        for (int i = 0; i < converter.countQuads(); ++i) {
                            add_quad(lines, quads, quadPts + i * 2, color, isIndexed,
                                     subpathIdxStart);
                        }
                        break;
                    }
                    case SkPath::kQuad_Verb: {
                        add_quad(lines, quads, pts, color, isIndexed, subpathIdxStart);
                        break;
                    }
                    case SkPath::kCubic_Verb: {
                        SkSTArray<15, SkPoint, true> quadPts;
                        GrPathUtils::convertCubicToQuads(pts, kTolerance, &quadPts);
                        int count = quadPts.count();
                        for (int i = 0; i < count; i += 3) {
                            add_quad(lines, quads, &quadPts[i], color, isIndexed, subpathIdxStart);
                        }
                        break;
                    }
                    case SkPath::kClose_Verb:
                        break;
                    case SkPath::kDone_Verb:
                        done = true;
                }
                first = false;
            }
        }
        return true;
    }

    // Lines and quads may render with an index buffer. However, we don't have any support for
    // overflowing the max index.
    static constexpr int kMaxIndexedVertexCnt = SK_MaxU16 / 3;
    struct PathInfo {
        GrColor  fColor;
        SkPath   fPath;
    };

    SkSTArray<1, PathInfo, true> fPaths;

    SkMatrix fViewMatrix;
    int fMaxLineVertices;
    int fMaxQuadVertices;
    bool fIsIndexed;

    typedef GrMeshDrawOp INHERITED;
};

bool GrMSAAPathRenderer::internalDrawPath(GrRenderTargetContext* renderTargetContext,
                                          const GrPaint& paint,
                                          GrAAType aaType,
                                          const GrUserStencilSettings& userStencilSettings,
                                          const GrClip& clip,
                                          const SkMatrix& viewMatrix,
                                          const GrShape& shape,
                                          bool stencilOnly) {
    SkASSERT(shape.style().isSimpleFill());
    SkPath path;
    shape.asPath(&path);

    static const int kMaxNumPasses = 2;

    int                          passCount = 0;
    const GrUserStencilSettings* passes[kMaxNumPasses];
    bool                         reverse = false;
    bool                         lastPassIsBounds;

    if (single_pass_shape(shape)) {
        passCount = 1;
        if (stencilOnly) {
            passes[0] = &gDirectToStencil;
        } else {
            passes[0] = &userStencilSettings;
        }
        lastPassIsBounds = false;
    } else {
        switch (path.getFillType()) {
            case SkPath::kInverseEvenOdd_FillType:
                reverse = true;
                // fallthrough
            case SkPath::kEvenOdd_FillType:
                passes[0] = &gEOStencilPass;
                if (stencilOnly) {
                    passCount = 1;
                    lastPassIsBounds = false;
                } else {
                    passCount = 2;
                    lastPassIsBounds = true;
                    if (reverse) {
                        passes[1] = &gInvEOColorPass;
                    } else {
                        passes[1] = &gEOColorPass;
                    }
                }
                break;

            case SkPath::kInverseWinding_FillType:
                reverse = true;
                // fallthrough
            case SkPath::kWinding_FillType:
                passes[0] = &gWindStencilSeparateWithWrap;
                passCount = 2;
                if (stencilOnly) {
                    lastPassIsBounds = false;
                    passCount = 1;
                } else {
                    lastPassIsBounds = true;
                    if (reverse) {
                        passes[1] = &gInvWindColorPass;
                    } else {
                        passes[1] = &gWindColorPass;
                    }
                }
                break;
            default:
                SkDEBUGFAIL("Unknown path fFill!");
                return false;
        }
    }

    SkRect devBounds;
    GetPathDevBounds(path, renderTargetContext->width(), renderTargetContext->height(), viewMatrix,
                     &devBounds);

    SkASSERT(passCount <= kMaxNumPasses);

    for (int p = 0; p < passCount; ++p) {
        if (lastPassIsBounds && (p == passCount-1)) {
            SkRect bounds;
            SkMatrix localMatrix = SkMatrix::I();
            if (reverse) {
                // draw over the dev bounds (which will be the whole dst surface for inv fill).
                bounds = devBounds;
                SkMatrix vmi;
                // mapRect through persp matrix may not be correct
                if (!viewMatrix.hasPerspective() && viewMatrix.invert(&vmi)) {
                    vmi.mapRect(&bounds);
                } else {
                    if (!viewMatrix.invert(&localMatrix)) {
                        return false;
                    }
                }
            } else {
                bounds = path.getBounds();
            }
            const SkMatrix& viewM = (reverse && viewMatrix.hasPerspective()) ? SkMatrix::I() :
                                                                               viewMatrix;
            sk_sp<GrDrawOp> op(GrRectOpFactory::MakeNonAAFill(paint.getColor(), viewM, bounds,
                                                              nullptr, &localMatrix));

            GrPipelineBuilder pipelineBuilder(paint, aaType);
            pipelineBuilder.setUserStencil(passes[p]);

            renderTargetContext->addDrawOp(pipelineBuilder, clip, std::move(op));
        } else {
            sk_sp<GrDrawOp> op = MSAAPathOp::Make(paint.getColor(), path, viewMatrix, devBounds);
            if (!op) {
                return false;
            }
            GrPipelineBuilder pipelineBuilder(paint, aaType);
            pipelineBuilder.setUserStencil(passes[p]);
            if (passCount > 1) {
                pipelineBuilder.setDisableColorXPFactory();
            }
            renderTargetContext->addDrawOp(pipelineBuilder, clip, std::move(op));
        }
    }
    return true;
}

bool GrMSAAPathRenderer::onCanDrawPath(const CanDrawPathArgs& args) const {
    // This path renderer only fills and relies on MSAA for antialiasing. Stroked shapes are
    // handled by passing on the original shape and letting the caller compute the stroked shape
    // which will have a fill style.
    return args.fShape->style().isSimpleFill() && (GrAAType::kCoverage != args.fAAType);
}

bool GrMSAAPathRenderer::onDrawPath(const DrawPathArgs& args) {
    GR_AUDIT_TRAIL_AUTO_FRAME(args.fRenderTargetContext->auditTrail(),
                              "GrMSAAPathRenderer::onDrawPath");
    SkTLazy<GrShape> tmpShape;
    const GrShape* shape = args.fShape;
    if (shape->style().applies()) {
        SkScalar styleScale = GrStyle::MatrixToScaleFactor(*args.fViewMatrix);
        tmpShape.init(args.fShape->applyStyle(GrStyle::Apply::kPathEffectAndStrokeRec, styleScale));
        shape = tmpShape.get();
    }
    return this->internalDrawPath(args.fRenderTargetContext,
                                  *args.fPaint,
                                  args.fAAType,
                                  *args.fUserStencilSettings,
                                  *args.fClip,
                                  *args.fViewMatrix,
                                  *shape,
                                  false);
}

void GrMSAAPathRenderer::onStencilPath(const StencilPathArgs& args) {
    GR_AUDIT_TRAIL_AUTO_FRAME(args.fRenderTargetContext->auditTrail(),
                              "GrMSAAPathRenderer::onStencilPath");
    SkASSERT(args.fShape->style().isSimpleFill());
    SkASSERT(!args.fShape->mayBeInverseFilledAfterStyling());

    GrPaint paint;
    paint.setXPFactory(GrDisableColorXPFactory::Make());

    this->internalDrawPath(args.fRenderTargetContext, paint, args.fAAType,
                           GrUserStencilSettings::kUnused, *args.fClip, *args.fViewMatrix,
                           *args.fShape, true);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
