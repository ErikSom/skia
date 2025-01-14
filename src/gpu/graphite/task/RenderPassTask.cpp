/*
 * Copyright 2021 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/graphite/task/RenderPassTask.h"

#include "src/gpu/graphite/Caps.h"
#include "src/gpu/graphite/CommandBuffer.h"
#include "src/gpu/graphite/ContextPriv.h"
#include "src/gpu/graphite/DrawPass.h"
#include "src/gpu/graphite/Log.h"
#include "src/gpu/graphite/ResourceProvider.h"
#include "src/gpu/graphite/ScratchResourceManager.h"
#include "src/gpu/graphite/Texture.h"
#include "src/gpu/graphite/TextureProxy.h"

namespace skgpu::graphite {

sk_sp<RenderPassTask> RenderPassTask::Make(DrawPassList passes,
                                           const RenderPassDesc& desc,
                                           sk_sp<TextureProxy> target) {
    // For now we have one DrawPass per RenderPassTask
    SkASSERT(passes.size() == 1);
    if (!target) {
        return nullptr;
    }

    if (desc.fColorAttachment.fTextureInfo.isValid()) {
        // The color attachment's samples count must ether match the render pass's samples count
        // or be 1 (when multisampled render to single sampled is used).
        SkASSERT(desc.fSampleCount == desc.fColorAttachment.fTextureInfo.numSamples() ||
                 1 == desc.fColorAttachment.fTextureInfo.numSamples());
    }

    if (desc.fDepthStencilAttachment.fTextureInfo.isValid()) {
        SkASSERT(desc.fSampleCount == desc.fDepthStencilAttachment.fTextureInfo.numSamples());
    }

    return sk_sp<RenderPassTask>(new RenderPassTask(std::move(passes), desc, target));
}

RenderPassTask::RenderPassTask(DrawPassList passes,
                               const RenderPassDesc& desc,
                               sk_sp<TextureProxy> target)
        : fDrawPasses(std::move(passes)), fRenderPassDesc(desc), fTarget(std::move(target)) {}

RenderPassTask::~RenderPassTask() = default;

Task::Status RenderPassTask::prepareResources(ResourceProvider* resourceProvider,
                                              ScratchResourceManager* scratchManager,
                                              const RuntimeEffectDictionary* runtimeDict) {
    SkASSERT(fTarget);
    // TODO: Use the scratch resource manager to instantiate the target
    if (!TextureProxy::InstantiateIfNotLazy(resourceProvider, fTarget.get())) {
        SKGPU_LOG_W("Failed to instantiate RenderPassTask target. Will not create renderpass!");
        SKGPU_LOG_W("Dimensions are (%d, %d).",
                    fTarget->dimensions().width(), fTarget->dimensions().height());
        return Status::kFail;
    }

    // Assuming one draw pass per renderpasstask for now
    SkASSERT(fDrawPasses.size() == 1);
    for (const auto& drawPass: fDrawPasses) {
        if (!drawPass->prepareResources(resourceProvider, runtimeDict, fRenderPassDesc)) {
            return Status::kFail;
        }
    }

    // Once all internal resources have been prepared and instantiated, reclaim any pending returns
    // from the scratch manager, since at the equivalent point in the task graph's addCommands()
    // phase, the renderpass will have sampled from any scratch textures and their contents no
    // longer have to be preserved.
    scratchManager->notifyResourcesConsumed();
    return Status::kSuccess;
}

Task::Status RenderPassTask::addCommands(Context* context,
                                         CommandBuffer* commandBuffer,
                                         ReplayTargetData replayData) {
    // TBD: Expose the surfaces that will need to be attached within the renderpass?

    // TODO: for task execution, start the render pass, then iterate passes and
    // possibly(?) start each subpass, and call DrawPass::addCommands() on the command buffer
    // provided to the task. Then close the render pass and we should have pixels..

    // Instantiate the target
    SkASSERT(fTarget && fTarget->isInstantiated());

    if (fTarget->texture() == replayData.fTarget) {
        commandBuffer->setReplayTranslation(replayData.fTranslation);
    } else {
        commandBuffer->clearReplayTranslation();
    }

    // We don't instantiate the MSAA or DS attachments in prepareResources because we want to use
    // the discardable attachments from the Context.
    ResourceProvider* resourceProvider = context->priv().resourceProvider();
    sk_sp<Texture> colorAttachment;
    sk_sp<Texture> resolveAttachment;
    if (fRenderPassDesc.fColorResolveAttachment.fTextureInfo.isValid()) {
        SkASSERT(fTarget->numSamples() == 1 &&
                 fRenderPassDesc.fColorAttachment.fTextureInfo.numSamples() > 1);
        colorAttachment = resourceProvider->findOrCreateDiscardableMSAAAttachment(
                fTarget->dimensions(), fRenderPassDesc.fColorAttachment.fTextureInfo);
        if (!colorAttachment) {
            SKGPU_LOG_W("Could not get Color attachment for RenderPassTask");
            return Status::kFail;
        }
        resolveAttachment = fTarget->refTexture();
    } else {
        colorAttachment = fTarget->refTexture();
    }

    sk_sp<Texture> depthStencilAttachment;
    if (fRenderPassDesc.fDepthStencilAttachment.fTextureInfo.isValid()) {
        // TODO: ensure this is a scratch/recycled texture
        SkASSERT(fTarget->isInstantiated());
        SkISize dimensions = context->priv().caps()->getDepthAttachmentDimensions(
                fTarget->texture()->textureInfo(), fTarget->dimensions());
        depthStencilAttachment = resourceProvider->findOrCreateDepthStencilAttachment(
                dimensions, fRenderPassDesc.fDepthStencilAttachment.fTextureInfo);
        if (!depthStencilAttachment) {
            SKGPU_LOG_W("Could not get DepthStencil attachment for RenderPassTask");
            return Status::kFail;
        }
    }

    // TODO(b/313629288) we always pass in the render target's dimensions as the viewport here.
    // Using the dimensions of the logical device that we're drawing to could reduce flakiness in
    // rendering.
    if (commandBuffer->addRenderPass(fRenderPassDesc,
                                     std::move(colorAttachment),
                                     std::move(resolveAttachment),
                                     std::move(depthStencilAttachment),
                                     SkRect::Make(fTarget->dimensions()),
                                     fDrawPasses)) {
        return Status::kSuccess;
    } else {
        return Status::kFail;
    }
}

} // namespace skgpu::graphite
