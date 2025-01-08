/*
 * Copyright (C) 2017-2024 Apple Inc. All rights reserved.
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

#include "CallFrame.h"
#include "VM.h"
#include "WeakGCSetInlines.h"
#include <wtf/DoublyLinkedList.h>
#include <wtf/Expected.h>
#include <wtf/IterationStatus.h>
#include <wtf/Lock.h>
#include <wtf/TZoneMalloc.h>

namespace JSC {

class VMInspector {
    WTF_MAKE_TZONE_ALLOCATED(VMInspector);
    WTF_MAKE_NONCOPYABLE(VMInspector);
    VMInspector() = default;
public:
    using VMSnapshot = HashSet<VM*>;

    enum class Error {
        None,
        TimedOut
    };

    JS_EXPORT_PRIVATE static VMInspector& singleton();

    void add(VM*);
    void remove(VM*);
    ALWAYS_INLINE static bool isValidVM(VM* vm)
    {
        return vm == m_recentVM ? true : isValidVMSlow(vm);
    }

    Lock& getLock() WTF_RETURNS_LOCK(m_lock) { return m_lock; }

    void iterate(const Invocable<IterationStatus(VM&)> auto& functor) WTF_REQUIRES_LOCK(m_lock)
    {
        for (VM* vm = m_vmList.head(); vm; vm = vm->next()) {
            IterationStatus status = functor(*vm);
            if (status == IterationStatus::Done)
                return;
        }
    }

    JS_EXPORT_PRIVATE static void forEachVM(Function<IterationStatus(VM&)>&&);
    JS_EXPORT_PRIVATE static void dumpVMs();

    // Returns null if the callFrame doesn't actually correspond to any active VM.
    JS_EXPORT_PRIVATE static VM* vmForCallFrame(CallFrame*);

    Expected<bool, Error> isValidExecutableMemory(void*) WTF_REQUIRES_LOCK(m_lock);
    Expected<CodeBlock*, Error> codeBlockForMachinePC(void*) WTF_REQUIRES_LOCK(m_lock);

    JS_EXPORT_PRIVATE static bool currentThreadOwnsJSLock(VM*);
    JS_EXPORT_PRIVATE static void gc(VM*);
    JS_EXPORT_PRIVATE static void edenGC(VM*);
    JS_EXPORT_PRIVATE static bool isInHeap(Heap*, void*);
    JS_EXPORT_PRIVATE static bool isValidCell(Heap*, JSCell*);
    JS_EXPORT_PRIVATE static bool isValidCodeBlock(VM*, CodeBlock*);
    JS_EXPORT_PRIVATE static CodeBlock* codeBlockForFrame(VM*, CallFrame* topCallFrame, unsigned frameNumber);
    JS_EXPORT_PRIVATE static void dumpCallFrame(VM*, CallFrame*, unsigned framesToSkip = 0);
    JS_EXPORT_PRIVATE static void dumpRegisters(CallFrame*);
    JS_EXPORT_PRIVATE static void dumpStack(VM*, CallFrame* topCallFrame, unsigned framesToSkip = 0);
    JS_EXPORT_PRIVATE static void dumpValue(JSValue);
    JS_EXPORT_PRIVATE static void dumpCellMemory(JSCell*);
    JS_EXPORT_PRIVATE static void dumpCellMemoryToStream(JSCell*, PrintStream&);
    JS_EXPORT_PRIVATE static void dumpSubspaceHashes(VM*);
    JS_EXPORT_PRIVATE unsigned vmCount();

#if USE(JSVALUE64)
    static bool verifyCell(VM&, JSCell*);
#endif

    void dumpLeaks();
    
    JS_EXPORT_PRIVATE void prepareForNewSnapshot();
    JS_EXPORT_PRIVATE void snapshotAdd(VM* vm);
    JS_EXPORT_PRIVATE void setCommonVM(VM* vm);

#define ADD_REMOVE_COUNT(name)                      \
    JS_EXPORT_PRIVATE void add##name(void*);        \
    JS_EXPORT_PRIVATE void remove##name(void*);     \
    JS_EXPORT_PRIVATE unsigned live##name##Count(); \
    UncheckedKeyHashSet<void*> m_live##name##Set;

    ADD_REMOVE_COUNT(Document)
    ADD_REMOVE_COUNT(Worker)
    ADD_REMOVE_COUNT(DOMWrapper)
    ADD_REMOVE_COUNT(ScriptWrapperable)
    ADD_REMOVE_COUNT(WasmMemory)
    ADD_REMOVE_COUNT(WasmModule)
    ADD_REMOVE_COUNT(WasmTable)

#define ADD_COUNT(name) \
    JS_EXPORT_PRIVATE void add##name(JSCell*); \
    JS_EXPORT_PRIVATE unsigned live##name##Count(VM*);

    ADD_COUNT(JSGlobalObject)
    ADD_COUNT(JSWorker)
    ADD_COUNT(JSFunction)
    ADD_COUNT(JSWebAssemblyInstance)
    ADD_COUNT(JSPromise)

    JS_EXPORT_PRIVATE void removeVM(VM*);
    
private:
    JS_EXPORT_PRIVATE static bool isValidVMSlow(VM*);
    JS_EXPORT_PRIVATE static void registerDumpLeak();

    Lock m_lock;
    DoublyLinkedList<VM> m_vmList WTF_GUARDED_BY_LOCK(m_lock);
    JS_EXPORT_PRIVATE static VM* m_recentVM;

public:
    JS_EXPORT_PRIVATE VM* m_commonVM;
    VMSnapshot m_currentSnapshot;
    Vector<VMSnapshot> m_globalSnapshots;

    class Data {
        WTF_MAKE_TZONE_ALLOCATED(Data);
        WTF_MAKE_NONCOPYABLE(Data);
    public:
        Data(VM& vm)
            : m_JSGlobalObjectSet(vm)
            , m_JSWorkerSet(vm)
            , m_JSFunctionSet(vm)
            , m_JSWebAssemblyInstanceSet(vm)
            , m_JSPromiseSet(vm)
        {
        }

        WeakGCSet<JSCell> m_JSGlobalObjectSet;
        WeakGCSet<JSCell> m_JSWorkerSet;
        WeakGCSet<JSCell> m_JSFunctionSet;
        WeakGCSet<JSCell> m_JSWebAssemblyInstanceSet;
        WeakGCSet<JSCell> m_JSPromiseSet;
    };
    
    UncheckedKeyHashMap<VM*, std::unique_ptr<Data>> m_data;
};

} // namespace JSC
