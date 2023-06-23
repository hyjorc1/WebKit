/*
 * Copyright (C) 2015, 2016 Apple Inc. All rights reserved.
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

// https://whatwg.github.io/loader/#loader-object
// Module Loader has several hooks that can be customized by the platform.
// For example, the [[Fetch]] hook can be provided by the JavaScriptCore shell
// as fetching the payload from the local file system.
// Currently, there are 4 hooks.
//    1. Loader.resolve
//    2. Loader.fetch

@linkTimeConstant
function setStateToMax(entry, newState)
{
    // https://whatwg.github.io/loader/#set-state-to-max

    "use strict";

    if (entry.state < newState)
        entry.state = newState;
}

@linkTimeConstant
function newRegistryEntry(key)
{
    // https://whatwg.github.io/loader/#registry
    //
    // Each registry entry becomes one of the 5 states.
    // 1. Fetch
    //     Ready to fetch (or now fetching) the resource of this module.
    //     Typically, we fetch the source code over the network or from the file system.
    //     a. If the status is Fetch and there is no entry.fetch promise, the entry is ready to fetch.
    //     b. If the status is Fetch and there is the entry.fetch promise, the entry is just fetching the resource.
    //
    // 2. Instantiate (AnalyzeModule)
    //     Ready to instantiate (or now instantiating) the module record from the fetched
    //     source code.
    //     Typically, we parse the module code, extract the dependencies and binding information.
    //     a. If the status is Instantiate and there is no entry.instantiate promise, the entry is ready to instantiate.
    //     b. If the status is Instantiate and there is the entry.fetch promise, the entry is just instantiating
    //        the module record.
    //
    // 3. Satisfy
    //     Ready to request the dependent modules (or now requesting & resolving).
    //     Without this state, the current draft causes infinite recursion when there is circular dependency.
    //     a. If the status is Satisfy and there is no entry.satisfy promise, the entry is ready to resolve the dependencies.
    //     b. If the status is Satisfy and there is the entry.satisfy promise, the entry is just resolving
    //        the dependencies.
    //
    // 4. Link
    //     Ready to link the module with the other modules.
    //     Linking means that the module imports and exports the bindings from/to the other modules.
    //
    // 5. Ready
    //     The module is linked, so the module is ready to be executed.
    //
    // Each registry entry has the 4 promises; "fetch", "instantiate" and "satisfy".
    // They are assigned when starting the each phase. And they are fulfilled when the each phase is completed.
    //
    // In the current module draft, linking will be performed after the whole modules are instantiated and the dependencies are resolved.
    // And execution is also done after the all modules are linked.
    //
    // TODO: We need to exploit the way to execute the module while fetching non-related modules.
    // One solution; introducing the ready promise chain to execute the modules concurrently while keeping
    // the execution order.

    "use strict";

    return {
        key: key,
        state: @ModuleFetch,
        fetch: @undefined,
        instantiate: @undefined,
        satisfy: @undefined,
        isPureSatisfy: false,
        dependencies: [], // To keep the module order, we store the module keys in the array.
        module: @undefined, // JSModuleRecord
        linkError: @undefined,
        linkSucceeded: true,
        evaluated: false,
        then: @undefined,
        isAsync: false,
        id: "NAN.js",
    };
}

@visibility=PrivateRecursive
function ensureRegistered(key)
{
    // https://whatwg.github.io/loader/#ensure-registered

    "use strict";

    var entry = this.registry.@get(key);
    if (entry)
        return entry;

    entry = @newRegistryEntry(key);
    this.registry.@set(key, entry);

    let size = this.registry.@size;
    entry.id = (size + ".js");
    return entry;
}

@linkTimeConstant
function forceFulfillPromise(promise, value)
{
    "use strict";

    @assert(@isPromise(promise));

    if ((@getPromiseInternalField(promise, @promiseFieldFlags) & @promiseStateMask) === @promiseStatePending)
        @fulfillPromise(promise, value);
}

@linkTimeConstant
function fulfillFetch(entry, source)
{
    // https://whatwg.github.io/loader/#fulfill-fetch

    "use strict";

    if (!entry.fetch)
        entry.fetch = @newPromiseCapability(@InternalPromise).@promise;
    @forceFulfillPromise(entry.fetch, source);
    @setStateToMax(entry, @ModuleInstantiate);
}

// Loader.

@visibility=PrivateRecursive
function requestFetch(entry, parameters, fetcher)
{
    // https://whatwg.github.io/loader/#request-fetch

    "use strict";

    if (entry.fetch) {
        var currentAttempt = entry.fetch;
        if (entry.state !== @ModuleFetch)
            return currentAttempt;

        return currentAttempt.catch((error) => {
            // Even if the existing fetching request failed, this attempt may succeed.
            // For example, previous attempt used invalid integrity="" value. But this
            // request could have the correct integrity="" value. In that case, we should
            // retry fetching for this request.
            // https://html.spec.whatwg.org/#fetch-a-single-module-script
            if (currentAttempt === entry.fetch)
                entry.fetch = @undefined;
            return this.requestFetch(entry, parameters, fetcher);
        });
    }

    // Hook point.
    // 2. Loader.fetch
    //     https://whatwg.github.io/loader/#browser-fetch
    //     Take the key and fetch the resource actually.
    //     For example, JavaScriptCore shell can provide the hook fetching the resource
    //     from the local file system.
    var fetchPromise = this.fetch(entry.key, parameters, fetcher).then((source) => {
        @setStateToMax(entry, @ModuleInstantiate);
        return source;
    });
    entry.fetch = fetchPromise;
    return fetchPromise;
}

@visibility=PrivateRecursive
function requestInstantiate(entry, parameters, fetcher, root)
{
    // https://whatwg.github.io/loader/#request-instantiate

    "use strict";
    @$vm.print("RI 1 - entry.key ", entry.id, " ", root);

    // entry.instantiate is set if fetch succeeds.
    if (entry.instantiate) {
        @$vm.print("RI 2 cached - entry.key ", entry.id, " ", root);
        return entry.instantiate; // Promise
    }

    var instantiatePromise = (async () => {
        var source;
        @$vm.print("RI 3 before await requestFetch - entry.key ", entry.id, " ", root);
        source = await this.requestFetch(entry, parameters, fetcher);
        @$vm.print("RI 4 after  await requestFetch - entry.key ", entry.id, " ", root);
        
        // https://html.spec.whatwg.org/#fetch-a-single-module-script
        // Now fetching request succeeds. Then even if instantiation fails, we should cache it.
        // Instantiation won't be retried.
        if (entry.instantiate) {
            @$vm.print("RI 5 cached before await entry.instantiate - entry.key ", entry.id, " ", root);
            let result = await entry.instantiate; // Promise.resolve(await entry.instantiate)
            @$vm.print("RI 6 cached after  await entry.instantiate - entry.key ", entry.id, " ", root);
            return result;
        }
        entry.instantiate = instantiatePromise;

        var key = entry.key;
        var moduleRecord = await this.parseModule(key, source);
        var dependenciesMap = moduleRecord.dependenciesMap;
        var requestedModules = this.requestedModules(moduleRecord);
        var dependencies = @newArrayWithSize(requestedModules.length);
        for (var i = 0, length = requestedModules.length; i < length; ++i) {
            var depName = requestedModules[i];
            var depKey = this.resolve(depName, key, fetcher);
            var depEntry = this.ensureRegistered(depKey);
            @putByValDirect(dependencies, i, depEntry);
            dependenciesMap.@set(depName, depEntry);
        }
        entry.dependencies = dependencies;
        entry.module = moduleRecord;
        @setStateToMax(entry, @ModuleSatisfy);
        @$vm.print("RI 7 return entry - entry.key ", entry.id, " ", root);
        return entry; // Promise.resolve(entry);
    })();
    return instantiatePromise; // Promise if resolved return entry
}

@visibility=PrivateRecursive
function requestSatisfyUtil(entry, parameters, fetcher, visited, root, impureSatisfies)
{
    // https://html.spec.whatwg.org/#internal-module-script-graph-fetching-procedure

    "use strict";
    @$vm.print("RS 1 - entry.key ", entry.id, " ", root);

    if (entry.satisfy) {
        @$vm.print("RS 2 cached - entry.key ", entry.id, " ", root);
        return entry.satisfy;
    }

    visited.@add(entry);
    var satisfyPromise = this.requestInstantiate(entry, parameters, fetcher, root).then((entry) => {
        @$vm.print("RS 3 >>>>>>>>>>>>>>>>>>>> RIed - entry.key ", entry.id, " ", root);

        if (entry.satisfy) {
            @$vm.print("RS 4 <<<<<<<<<<<<<<<<<<<< cached - entry.key ", entry.id, " ", root);
            return entry.satisfy;
        }

        var depLoads = this.requestedModuleParameters(entry.module);

        var str = " ### " + entry.id + " -> [ ";

        for (var i = 0, length = entry.dependencies.length; i < length; ++i) {
            var parameters = depLoads[i];
            var depEntry = entry.dependencies[i];
            var promise;
            var promiseStr = "";

            if (visited.@has(depEntry)) {
                @$vm.print("RS 5 need RI for dep - entry.key ", entry.id, " ", depEntry.id, " ", root);
                promise = this.requestInstantiate(depEntry, parameters, fetcher, root);
                promiseStr = "RI";
            } else {
                // Currently, module loader do not pass any information for non-top-level module fetching.
                @$vm.print("RS 6 need RS for dep - entry.key ", entry.id, " ", depEntry.id, " ", root);
                promise = this.requestSatisfyUtil(depEntry, parameters, fetcher, visited, root, impureSatisfies);
                promiseStr = "RS";
            }
            str +=  promiseStr + "(" + depEntry.id + "), ";
            @putByValDirect(depLoads, i, promise);
        }
        str += "]";

        @$vm.print(str);
        @$vm.print("RS 9 <<<<<<<<<<<<<<<<<<<< return satisfyingPromise - entry.key ", entry.id, " ", root);

        return @InternalPromise.internalAll(depLoads).then((depEntries) => {
            if (entry.satisfy) {
                return entry;
            }

            entry.isPureSatisfy = true;
            for (var j = 0, length = depEntries.length; j < length; ++j) {
                if (!depEntries[j].isPureSatisfy) {
                    entry.isPureSatisfy = false;
                    break;
                }
            }
            
            if (entry.isPureSatisfy) {
                // @$vm.print("RS 10 all depLoads done 1 - entry.key ", entry.id, " ", root);
                @setStateToMax(entry, @ModuleLink);
                entry.satisfy = satisfyPromise;
            } else {
                impureSatisfies.@add(entry);
            }
            return entry;
        });
    });

    return satisfyPromise;
}

@visibility=PrivateRecursive
function requestSatisfy(entry, parameters, fetcher, visited, root)
{
    "use strict";

    var impureSatisfies = new @Set;
    return this.requestSatisfyUtil(entry, parameters, fetcher, visited, root, impureSatisfies).then((entry) => {
        for (var impureSatisfy of impureSatisfies) {
            @setStateToMax(impureSatisfy, @ModuleLink);
            impureSatisfy.satisfy = (async () => {
                return impureSatisfy;
            })();
            impureSatisfy.isPureSatisfy = true;
        }
        return entry;
    });
}

// Linking semantics.

@visibility=PrivateRecursive
function link(entry, fetcher)
{
    // https://html.spec.whatwg.org/#fetch-the-descendants-of-and-instantiate-a-module-script

    "use strict";

    if (!entry.linkSucceeded)
        throw entry.linkError;
    if (entry.state < @ModuleLink)
        @throwTypeError("Requested module is not instantiated yet.");
    if (entry.state === @ModuleReady)
        return;
    @setStateToMax(entry, @ModuleReady);

    try {
        // Since we already have the "dependencies" field,
        // we can call moduleDeclarationInstantiation with the correct order
        // without constructing the dependency graph by calling dependencyGraph.
        var hasAsyncDependency = false;
        var dependencies = entry.dependencies;
        for (var i = 0, length = dependencies.length; i < length; ++i) {
            var dependency = dependencies[i];
            this.link(dependency, fetcher);
            hasAsyncDependency ||= dependency.isAsync;
        }

        entry.isAsync = this.moduleDeclarationInstantiation(entry.module, fetcher) || hasAsyncDependency;
    } catch (error) {
        entry.linkSucceeded = false;
        entry.linkError = error;
        throw error;
    }
}

// Module semantics.

@visibility=PrivateRecursive
function moduleEvaluation(entry, fetcher)
{
    // http://www.ecma-international.org/ecma-262/6.0/#sec-moduleevaluation
    "use strict";

    if (entry.evaluated)
        return;
    entry.evaluated = true;

    // The contents of the [[RequestedModules]] is cloned into entry.dependencies.
    var dependencies = entry.dependencies;

    if (!entry.isAsync) {
        // Since linking sets isAsync for any strongly connected component with an async module we should only get here if all our dependencies are also sync.
        for (var i = 0, length = dependencies.length; i < length; ++i) {
            var dependency = dependencies[i];
            @assert(!dependency.isAsync);
            this.moduleEvaluation(dependency, fetcher);
        }

        this.evaluate(entry.key, entry.module, fetcher);
    } else
        return this.asyncModuleEvaluation(entry, fetcher, dependencies);
}

@visibility=PrivateRecursive
async function asyncModuleEvaluation(entry, fetcher, dependencies)
{
    "use strict";

    for (var i = 0, length = dependencies.length; i < length; ++i)
        await this.moduleEvaluation(dependencies[i], fetcher);

    var resumeMode = @GeneratorResumeModeNormal;
    while (true) {
        var awaitedValue = this.evaluate(entry.key, entry.module, fetcher, awaitedValue, resumeMode);
        if (@getAbstractModuleRecordInternalField(entry.module, @abstractModuleRecordFieldState) == @GeneratorStateExecuting)
            return;

        try {
            awaitedValue = await awaitedValue;
            resumeMode = @GeneratorResumeModeNormal;
        } catch (e) {
            awaitedValue = e;
            resumeMode = @GeneratorResumeModeThrow;
        }
    }
}

// APIs to control the module loader.

@visibility=PrivateRecursive
function provideFetch(key, value)
{
    "use strict";

    var entry = this.ensureRegistered(key);

    if (entry.state > @ModuleFetch)
        @throwTypeError("Requested module is already fetched.");
    @fulfillFetch(entry, value);
}

@visibility=PrivateRecursive
async function loadModule(key, parameters, fetcher)
{
    "use strict";

    var importMap = @importMapStatus();
    if (importMap)
        await importMap;
    var entry = this.ensureRegistered(key);
    @$vm.print("loadModule 1 -> requestSatisfy ", entry.id);   // <- 1 call (first one)
    entry = await this.requestSatisfy(entry, parameters, fetcher, new @Set, entry.id);
    return entry.key;
}

@visibility=PrivateRecursive
function linkAndEvaluateModule(key, fetcher)
{
    "use strict";

    var entry = this.ensureRegistered(key);
    @$vm.print("linkAndEvaluateModule 1 ", entry.id);
    this.link(entry, fetcher);
    return this.moduleEvaluation(entry, fetcher);
}

@visibility=PrivateRecursive
async function loadAndEvaluateModule(moduleName, parameters, fetcher)
{
    "use strict";

    var importMap = @importMapStatus();
    if (importMap)
        await importMap;
    var key = this.resolve(moduleName, @undefined, fetcher);
    var entry = this.ensureRegistered(key);
    @$vm.print("loadAndEvaluateModule 1 -> loadModule ", entry.id);
    key = await this.loadModule(key, parameters, fetcher);
    @$vm.print("loadAndEvaluateModule 2 -> linkAndEvaluateModule ", entry.id);
    return await this.linkAndEvaluateModule(key, fetcher);
}

@visibility=PrivateRecursive
async function requestImportModule(moduleName, referrer, parameters, fetcher)
{
    "use strict";

    var importMap = @importMapStatus();
    if (importMap)
        await importMap;
    var key = this.resolve(moduleName, referrer, fetcher);
    var entry = this.ensureRegistered(key);
    @$vm.print("requestImportModule 1 -> requestSatisfy ", entry.id);
    entry = await this.requestSatisfy(entry, parameters, fetcher, new @Set, entry.id);
    @$vm.print("requestImportModule 2 -> linkAndEvaluateModule ", entry.id);
    await this.linkAndEvaluateModule(entry.key, fetcher);
    return this.getModuleNamespaceObject(entry.module);
}

@visibility=PrivateRecursive
function dependencyKeysIfEvaluated(key)
{
    "use strict";

    var entry = this.registry.@get(key);
    if (!entry || !entry.evaluated)
        return null;

    var dependencies = entry.dependencies;
    var length = dependencies.length;
    var result = new @Array(length);
    for (var i = 0; i < length; ++i)
        result[i] = dependencies[i].key;

    return result;
}
