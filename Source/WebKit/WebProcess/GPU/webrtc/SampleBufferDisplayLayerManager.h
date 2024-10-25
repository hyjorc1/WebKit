/*
 * Copyright (C) 2020 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if PLATFORM(COCOA) && ENABLE(GPU_PROCESS) && ENABLE(MEDIA_STREAM)

#include "SampleBufferDisplayLayer.h"
#include <wtf/HashMap.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeWeakPtr.h>

namespace WebKit {

class GPUProcessConnection;

class SampleBufferDisplayLayerManager : public CanMakeWeakPtr<SampleBufferDisplayLayerManager> {
    WTF_MAKE_TZONE_ALLOCATED(SampleBufferDisplayLayerManager);
public:
    SampleBufferDisplayLayerManager(GPUProcessConnection&);
    ~SampleBufferDisplayLayerManager() = default;

    void ref() const;
    void deref() const;

    void addLayer(SampleBufferDisplayLayer&);
    void removeLayer(SampleBufferDisplayLayer&);

    void didReceiveLayerMessage(IPC::Connection&, IPC::Decoder&);
    RefPtr<WebCore::SampleBufferDisplayLayer> createLayer(WebCore::SampleBufferDisplayLayerClient&);

private:
    ThreadSafeWeakPtr<GPUProcessConnection> m_gpuProcessConnection;
    HashMap<SampleBufferDisplayLayerIdentifier, WeakPtr<SampleBufferDisplayLayer>> m_layers;
};

}

#endif // PLATFORM(COCOA) && ENABLE(GPU_PROCESS) && ENABLE(MEDIA_STREAM)
