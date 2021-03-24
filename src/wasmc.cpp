/* Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasmc.h"
#include "debugging.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cinttypes>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>
#include <set>
#include <map>
#include <mutex> // For std::unique_lock
#include <shared_mutex>
#include <atomic>
#define PERF_TIME

#if HERA_WASMER
#include "c-api/wasmer_wasm.h"
#else
#include "wasmtime.h"
#define MAX_INSTANCE 10000
#endif
#define own

#ifdef PERF_TIME
#include <chrono>
using namespace std::chrono;
#endif

using namespace std;

const char *OUT_OF_GAS = "Out of gas.";
const char *REVERT = "revert";
const char *FINISH = "finish";
const char *MEMORY_ACCESS = "memory access";
const char *UNREACHABLE = "unreachable";
const char *STACK_OVERFLOW = "stack exhausted";
const string BCOS_MODULE_NAME = "bcos";
const string DEBUG_MODULE_NAME = "debug";
const string ETHEREUM_MODULE_NAME = "ethereum";

namespace hera
{
#if HERA_WASMER
    // Use the last_error API to retrieve error messages
    string get_last_wasmer_error()
    {
        int error_len = wasmer_last_error_length();
        string errorMessage((size_t)error_len, '\0');
        wasmer_last_error_message(const_cast<char *>(errorMessage.data()), error_len);
        return errorMessage;
    }
#else
    string get_wasmtime_error(const char *message, wasmtime_error_t *error, wasm_trap_t *trap)
    {
        HERA_DEBUG << "error: " << message << "\n";
        wasm_byte_vec_t error_message;
        if (error != NULL)
        {
            wasmtime_error_message(error, &error_message);
            wasmtime_error_delete(error);
        }
        else
        {
            wasm_trap_message(trap, &error_message);
            wasm_trap_delete(trap);
        }
        string errorMessage((size_t)error_message.size, '\0');
        memcpy(const_cast<char *>(errorMessage.data()), error_message.data, error_message.size);
        wasm_byte_vec_delete(&error_message);
        return errorMessage;
    }
#endif
    struct ImportFunction
    {
        shared_ptr<wasm_functype_t> functionType;
        wasm_func_callback_with_env_t function;
    };
    struct WasmInstance
    {
        WasmInstance(shared_ptr<vector<shared_ptr<wasm_func_t> > > _functions, wasm_instance_t *_instance)
            : functions(_functions), instance(_instance) {}
        shared_ptr<vector<shared_ptr<wasm_func_t> > > functions = nullptr;
        wasm_instance_t *instance = nullptr;
        wasm_func_t *hashTypeFunc = nullptr;
        wasm_func_t *deployFunc = nullptr;
        wasm_func_t *mainFunc = nullptr;
        wasm_memory_t *memory = nullptr;
        wasm_extern_vec_t exports;
        atomic_bool idle = {true};
    };
    struct InstanceHolder
    {
        ~InstanceHolder() { instance->idle.store(true); }
        shared_ptr<WasmInstance> instance;
    };
    struct WasmInstanceContainer
    {
        wasm_engine_t *engine = nullptr;
        wasm_store_t *store = nullptr;
        wasm_module_t *module = nullptr;
        vector<shared_ptr<WasmInstance> > instances;
        std::shared_mutex instancesMutex;
    };
    class WasmcInterface : public EthereumInterface
    {
    public:
        explicit WasmcInterface(evmc::HostContext &_context, bytes_view _code,
                                evmc_message const &_msg, ExecutionResult &_result, bool _meterGas)
            : EthereumInterface(_context, _code, _msg, _result, _meterGas)
        {
        }

        void setWasmMemory(wasm_memory_t *_wasmMemory) { m_wasmMemory = _wasmMemory; }
        void setWasmStore(wasm_store_t *_wasmStore)
        {
            m_wasmStore = _wasmStore;
        }

        int64_t gesLeft()
        {
            return m_result.gasLeft;
        }
        own wasm_trap_t *beiRevertOrFinish(bool revert, uint32_t offset, uint32_t size)
        {
            HERA_DEBUG << (revert ? "revert " : "finish ") << hex << memorySize() << " " << offset << " " << size << dec << "\n";

            if (size != 0)
            {
                ensureSourceMemoryBounds(offset, size);
                m_result.returnValue = bytes(size, '\0');
                loadMemory(offset, m_result.returnValue, size);
            }

            m_result.isRevert = revert;
            own wasm_name_t message;
            if (revert)
            {
                wasm_name_new_from_string_nt(&message, REVERT);
            }
            else
            {
                wasm_name_new_from_string_nt(&message, FINISH);
            }
            own wasm_trap_t *trap = wasm_trap_new((wasm_store_t *)m_wasmStore, &message);
            wasm_name_delete(&message);
            return trap;
        }

    private:
        // These assume that m_wasmMemory was set prior to execution.
        size_t memorySize() const override { return wasm_memory_data_size(m_wasmMemory); }
        uint32_t memoryPages() const { return wasm_memory_size(m_wasmMemory); }
        void memorySet(size_t offset, uint8_t value) override
        {
            auto data = wasm_memory_data(m_wasmMemory);
            ensureCondition(data != NULL, InvalidMemoryAccess, string("memorySet failed"));
            data[offset] = (char)value;
        }
        uint8_t memoryGet(size_t offset) override
        {
            ensureCondition(memorySize() >= offset, InvalidMemoryAccess,
                            "Memory is shorter than requested segment");
            auto data = wasm_memory_data(m_wasmMemory);
            ensureCondition(data != NULL, InvalidMemoryAccess, string("memoryGet failed"));
            return (uint8_t)data[offset];
        }
        uint8_t *memoryPointer(size_t offset, size_t length) override
        {
            ensureCondition(memorySize() >= (offset + length), InvalidMemoryAccess,
                            "Memory is shorter than requested segment");
            auto data = wasm_memory_data(m_wasmMemory) + offset;
            return (uint8_t *)data;
        }

        wasm_memory_t *m_wasmMemory;
        wasm_store_t *m_wasmStore = nullptr;
    };

    unique_ptr<WasmEngine> WasmcEngine::create()
    {
        return unique_ptr<WasmEngine>{new WasmcEngine};
    }
    namespace
    {

        static inline own wasm_functype_t *wasm_functype_new_4_0(
            own wasm_valtype_t *p1, own wasm_valtype_t *p2, own wasm_valtype_t *p3, own wasm_valtype_t *p4)
        {
            wasm_valtype_t *ps[4] = {p1, p2, p3, p4};
            wasm_valtype_vec_t params, results;
            wasm_valtype_vec_new(&params, 4, ps);
            wasm_valtype_vec_new_empty(&results);
            return wasm_functype_new(&params, &results);
        }
        static inline own wasm_functype_t *wasm_functype_new_7_0(
            own wasm_valtype_t *p1, own wasm_valtype_t *p2, own wasm_valtype_t *p3, own wasm_valtype_t *p4, own wasm_valtype_t *p5, own wasm_valtype_t *p6, own wasm_valtype_t *p7)
        {
            wasm_valtype_vec_t params, results;
            wasm_valtype_t *ps[7] = {p1, p2, p3, p4, p5, p6, p7};
            wasm_valtype_vec_new(&params, 7, ps);
            wasm_valtype_vec_new_empty(&results);
            return wasm_functype_new(&params, &results);
        }
        static inline own wasm_functype_t *wasm_functype_new_4_1(
            own wasm_valtype_t *p1, own wasm_valtype_t *p2, own wasm_valtype_t *p3, own wasm_valtype_t *p4, own wasm_valtype_t *r)
        {
            wasm_valtype_vec_t params, results;
            wasm_valtype_t *ps[4] = {p1, p2, p3, p4};
            wasm_valtype_t *rs[1] = {r};
            wasm_valtype_vec_new(&params, 4, ps);
            wasm_valtype_vec_new(&results, 1, rs);
            return wasm_functype_new(&params, &results);
        }
        static inline own wasm_functype_t *wasm_functype_new_5_1(
            own wasm_valtype_t *p1, own wasm_valtype_t *p2, own wasm_valtype_t *p3, own wasm_valtype_t *p4, own wasm_valtype_t *p5, own wasm_valtype_t *r)
        {
            wasm_valtype_vec_t params, results;
            wasm_valtype_t *ps[5] = {p1, p2, p3, p4, p5};
            wasm_valtype_t *rs[1] = {r};
            wasm_valtype_vec_new(&params, 5, ps);
            wasm_valtype_vec_new(&results, 1, rs);
            return wasm_functype_new(&params, &results);
        }
        static inline own wasm_functype_t *wasm_functype_new_6_1(
            own wasm_valtype_t *p1, own wasm_valtype_t *p2, own wasm_valtype_t *p3, own wasm_valtype_t *p4, own wasm_valtype_t *p5, own wasm_valtype_t *p6, own wasm_valtype_t *r)
        {
            wasm_valtype_vec_t params, results;
            wasm_valtype_t *ps[6] = {p1, p2, p3, p4, p5, p6};
            wasm_valtype_t *rs[1] = {r};
            wasm_valtype_vec_new(&params, 6, ps);
            wasm_valtype_vec_new(&results, 1, rs);
            return wasm_functype_new(&params, &results);
        }
        static inline own wasm_functype_t *wasm_functype_new_7_1(
            own wasm_valtype_t *p1, own wasm_valtype_t *p2, own wasm_valtype_t *p3, own wasm_valtype_t *p4, own wasm_valtype_t *p5, own wasm_valtype_t *p6, own wasm_valtype_t *p7, own wasm_valtype_t *r)
        {
            wasm_valtype_vec_t params, results;
            wasm_valtype_t *ps[7] = {p1, p2, p3, p4, p5, p6, p7};
            wasm_valtype_t *rs[1] = {r};
            wasm_valtype_vec_new(&params, 7, ps);
            wasm_valtype_vec_new(&results, 1, rs);
            return wasm_functype_new(&params, &results);
        }

        WasmcInterface *getInterfaceFromEnv(void *env)
        {
            return (WasmcInterface *)env;
        }

        own wasm_trap_t *beiUseGas(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto gas = args->data[0].of.i64;
            HERA_DEBUG << " useGas " << gas << ", left=" << interface->gesLeft() << "\n";
            if (gas < 0)
            {
                own wasm_name_t message;
                wasm_name_new_from_string_nt(&message, "Negative gas supplied.");
                own wasm_trap_t *trap = wasm_trap_new((wasm_store_t *)env, &message);
                wasm_name_delete(&message);
                return trap;
            }
            interface->eeiUseGas(gas);
            if (interface->eeiGetGasLeft() < 0)
            {
                own wasm_name_t message;
                wasm_name_new_from_string_nt(&message, OUT_OF_GAS);
                own wasm_trap_t *trap = wasm_trap_new((wasm_store_t *)env, &message);
                wasm_name_delete(&message);
                return trap;
            }
            return NULL;
        }
        own wasm_trap_t *eeiGetGasLeft(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            results->data[0].kind = WASM_I64;
            results->data[0].of.i64 = (int64_t)interface->eeiGetGasLeft();
            return NULL;
        }
        own wasm_trap_t *eeiGetAddress(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto resultOffset = (uint32_t)args->data[0].of.i32;
            interface->eeiGetAddress(resultOffset);
            return NULL;
        }
        own wasm_trap_t *eeiGetExternalBalance(
            void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto addressOffset = (uint32_t)args->data[0].of.i32;
            auto resultOffset = (uint32_t)args->data[1].of.i32;
            interface->eeiGetExternalBalance(addressOffset, resultOffset);
            return NULL;
        }

        own wasm_trap_t *eeiGetBlockHash(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto number = (uint64_t)args->data[0].of.i64;
            auto resultOffset = (uint32_t)args->data[1].of.i32;
            results->data[0].kind = WASM_I32;
            results->data[0].of.i32 = (int32_t)interface->eeiGetBlockHash(number, resultOffset);
            return NULL;
        }
        own wasm_trap_t *eeiGetCallDataSize(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            results->data[0].kind = WASM_I32;
            results->data[0].of.i32 = (int32_t)interface->eeiGetCallDataSize();
            return NULL;
        }
        own wasm_trap_t *eeiCallDataCopy(
            void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto resultOffset = (uint32_t)args->data[0].of.i32;
            auto dataOffset = (uint32_t)args->data[1].of.i32;
            auto length = (uint32_t)args->data[2].of.i32;
            interface->eeiCallDataCopy(resultOffset, dataOffset, length);
            return NULL;
        }
        own wasm_trap_t *eeiGetCaller(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto resultOffset = (uint32_t)args->data[0].of.i32;
            interface->eeiGetCaller(resultOffset);
            return NULL;
        }
        own wasm_trap_t *eeiGetCallValue(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto resultOffset = (uint32_t)args->data[0].of.i32;
            interface->eeiGetCallValue(resultOffset);
            return NULL;
        }
        own wasm_trap_t *eeiCodeCopy(
            void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto resultOffset = (uint32_t)args->data[0].of.i32;
            auto codeOffset = (uint32_t)args->data[1].of.i32;
            auto length = (uint32_t)args->data[2].of.i32;
            interface->eeiCodeCopy(resultOffset, codeOffset, length);
            return NULL;
        }
        own wasm_trap_t *eeiGetCodeSize(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            results->data[0].kind = WASM_I32;
            results->data[0].of.i32 = (int32_t)interface->eeiGetCodeSize();
            return NULL;
        }
        own wasm_trap_t *eeiExternalCodeCopy(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto addressOffset = (uint32_t)args->data[0].of.i32;
            auto resultOffset = (uint32_t)args->data[1].of.i32;
            auto codeOffset = (uint32_t)args->data[2].of.i32;
            auto length = (uint32_t)args->data[3].of.i32;
            interface->eeiExternalCodeCopy(addressOffset, resultOffset, codeOffset, length);
            return NULL;
        }
        own wasm_trap_t *eeiGetExternalCodeSize(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto addressOffset = (uint32_t)args->data[0].of.i32;
            results->data[0].kind = WASM_I32;
            results->data[0].of.i32 = (int32_t)interface->eeiGetExternalCodeSize(addressOffset);
            return NULL;
        }
        own wasm_trap_t *eeiGetBlockCoinbase(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto resultOffset = (uint32_t)args->data[0].of.i32;
            interface->eeiGetBlockCoinbase(resultOffset);
            return NULL;
        }
        own wasm_trap_t *eeiGetBlockDifficulty(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto offset = (uint32_t)args->data[0].of.i32;
            interface->eeiGetBlockDifficulty(offset);
            return NULL;
        }
        own wasm_trap_t *eeiGetBlockGasLimit(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            results->data[0].kind = WASM_I64;
            results->data[0].of.i64 = (int64_t)interface->eeiGetBlockGasLimit();
            return NULL;
        }
        own wasm_trap_t *eeiGetTxGasPrice(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto valueOffset = (uint32_t)args->data[0].of.i32;
            interface->eeiGetTxGasPrice(valueOffset);
            return NULL;
        }
        own wasm_trap_t *eeiLog(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto dataOffset = (uint32_t)args->data[0].of.i32;
            auto length = (uint32_t)args->data[1].of.i32;
            auto numberOfTopics = (uint32_t)args->data[2].of.i32;
            auto topic1 = (uint32_t)args->data[3].of.i32;
            auto topic2 = (uint32_t)args->data[4].of.i32;
            auto topic3 = (uint32_t)args->data[5].of.i32;
            auto topic4 = (uint32_t)args->data[6].of.i32;
            interface->eeiLog(dataOffset, length, numberOfTopics, topic1, topic2, topic3, topic4);
            return NULL;
        }
        own wasm_trap_t *eeiGetBlockNumber(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            results->data[0].kind = WASM_I64;
            results->data[0].of.i64 = (int64_t)interface->eeiGetBlockNumber();
            return NULL;
        }
        own wasm_trap_t *eeiGetBlockTimestamp(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            results->data[0].kind = WASM_I64;
            results->data[0].of.i64 = (int64_t)interface->eeiGetBlockTimestamp();
            return NULL;
        }
        own wasm_trap_t *eeiGetTxOrigin(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto resultOffset = (uint32_t)args->data[0].of.i32;
            interface->eeiGetTxOrigin(resultOffset);
            return NULL;
        }
        own wasm_trap_t *eeiStorageStore(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto pathOffset = (uint32_t)args->data[0].of.i32;
            auto valueOffset = (uint32_t)args->data[1].of.i32;
            interface->eeiStorageStore(pathOffset, valueOffset);
            return NULL;
        }
        own wasm_trap_t *eeiStorageLoad(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto pathOffset = (uint32_t)args->data[0].of.i32;
            auto resultOffset = (uint32_t)args->data[1].of.i32;
            interface->eeiStorageLoad(pathOffset, resultOffset);
            return NULL;
        }
        own wasm_trap_t *beiSetStorage(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto keyOffset = (uint32_t)args->data[0].of.i32;
            auto keyLength = (uint32_t)args->data[1].of.i32;
            auto valueOffset = (uint32_t)args->data[2].of.i32;
            auto valueLength = (uint32_t)args->data[3].of.i32;
            interface->beiSetStorage(keyOffset, keyLength, valueOffset, valueLength);
            return NULL;
        }
        own wasm_trap_t *beiGetStorage(
            void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            const int32_t maxLength = 19264;
            auto keyOffset = (uint32_t)args->data[0].of.i32;
            auto keyLength = (uint32_t)args->data[1].of.i32;
            auto valueOffset = (uint32_t)args->data[2].of.i32;
            // auto valueLength = (uint32_t)args->data[3].of.i32;
            results->data[0].kind = WASM_I32;
            results->data[0].of.i32 = (int32_t)interface->beiGetStorage(keyOffset, keyLength, valueOffset, maxLength);
            return NULL;
        }
        own wasm_trap_t *beiGetCallData(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto resultOffset = (uint32_t)args->data[0].of.i32;
            interface->eeiCallDataCopy(resultOffset, 0, interface->eeiGetCallDataSize());
            return NULL;
        }
        own wasm_trap_t *eeiFinish(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto offset = (uint32_t)args->data[0].of.i32;
            auto size = (uint32_t)args->data[1].of.i32;
            return interface->beiRevertOrFinish(false, offset, size);
        }
        own wasm_trap_t *eeiRevert(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto offset = (uint32_t)args->data[0].of.i32;
            auto size = (uint32_t)args->data[1].of.i32;
            return interface->beiRevertOrFinish(true, offset, size);
        }
        own wasm_trap_t *beiCall(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto addressOffset = (uint32_t)args->data[0].of.i32;
            auto dataOffset = (uint32_t)args->data[1].of.i32;
            auto dataLength = (uint32_t)args->data[2].of.i32;
            results->data[0].kind = WASM_I32;
            results->data[0].of.i32 = (int32_t)interface->eeiCall(EthereumInterface::EEICallKind::Call, interface->eeiGetGasLeft(), addressOffset, 0, dataOffset, dataLength);
            return NULL;
        }
        own wasm_trap_t *eeiGetReturnDataSize(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            results->data[0].kind = WASM_I32;
            results->data[0].of.i32 = (int32_t)interface->eeiGetReturnDataSize();
            return NULL;
        }
        own wasm_trap_t *eeiReturnDataCopy(
            void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto dataOffset = (uint32_t)args->data[0].of.i32;
            auto offset = (uint32_t)args->data[1].of.i32;
            auto size = (uint32_t)args->data[2].of.i32;
            interface->eeiReturnDataCopy(dataOffset, offset, size);
            return NULL;
        }
        own wasm_trap_t *beiReturnDataCopy(
            void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto dataOffset = (uint32_t)args->data[0].of.i32;
            interface->eeiReturnDataCopy(dataOffset, 0, interface->eeiGetReturnDataSize());
            return NULL;
        }
        own wasm_trap_t *eeiCreate(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto valueOffset = (uint32_t)args->data[0].of.i32;
            auto dataOffset = (uint32_t)args->data[1].of.i32;
            auto length = (uint32_t)args->data[2].of.i32;
            auto resultOffset = (uint32_t)args->data[3].of.i32;
            results->data[0].kind = WASM_I32;
            results->data[0].of.i32 = (int32_t)interface->eeiCreate(valueOffset, dataOffset, length, resultOffset);
            return NULL;
        }
        own wasm_trap_t *eeiSelfDestruct(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto addressOffset = (uint32_t)args->data[0].of.i32;
            interface->eeiSelfDestruct(addressOffset);
            return NULL;
        }
        own wasm_trap_t *beiRegisterAsset(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto assetnameOffset = (uint32_t)args->data[0].of.i32;
            auto length = (uint32_t)args->data[1].of.i32;
            auto addressOffset = (uint32_t)args->data[2].of.i32;
            auto fungible = (int32_t)args->data[3].of.i32;
            auto total = (uint64_t)args->data[4].of.i64;
            auto descriptionOffset = (uint32_t)args->data[5].of.i32;
            auto descriptionLength = (uint32_t)args->data[6].of.i32;
            results->data[0].kind = WASM_I32;
            results->data[0].of.i32 = (int32_t)interface->beiRegisterAsset(assetnameOffset, length, addressOffset, fungible, total, descriptionOffset, descriptionLength);
            return NULL;
        }
        own wasm_trap_t *beiIssueFungibleAsset(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto addressOffset = (uint32_t)args->data[0].of.i32;
            auto assetnameOffset = (uint32_t)args->data[1].of.i32;
            auto length = (uint32_t)args->data[2].of.i32;
            auto amount = (uint64_t)args->data[3].of.i64;
            results->data[0].kind = WASM_I32;
            results->data[0].of.i32 = (int32_t)interface->beiIssueFungibleAsset(addressOffset, assetnameOffset, length, amount);
            return NULL;
        }
        own wasm_trap_t *beiIssueNotFungibleAsset(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto addressOffset = (uint32_t)args->data[0].of.i32;
            auto assetnameOffset = (uint32_t)args->data[1].of.i32;
            auto length = (uint32_t)args->data[2].of.i32;
            auto uriOffset = (uint32_t)args->data[3].of.i32;
            auto uriLength = (uint32_t)args->data[4].of.i32;
            results->data[0].kind = WASM_I64;
            results->data[0].of.i64 = (int64_t)interface->beiIssueNotFungibleAsset(addressOffset, assetnameOffset, length, uriOffset, uriLength);
            return NULL;
        }
        own wasm_trap_t *beiTransferAsset(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto addressOffset = (uint32_t)args->data[0].of.i32;
            auto assetnameOffset = (uint32_t)args->data[1].of.i32;
            auto length = (uint32_t)args->data[2].of.i32;
            auto amountOrID = (uint64_t)args->data[3].of.i64;
            auto fromSelf = (int32_t)args->data[4].of.i32;
            results->data[0].kind = WASM_I32;
            results->data[0].of.i32 = (int32_t)interface->beiTransferAsset(addressOffset, assetnameOffset, length, amountOrID, fromSelf);
            return NULL;
        }
        own wasm_trap_t *beiGetAssetBalance(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto addressOffset = (uint32_t)args->data[0].of.i32;
            auto assetnameOffset = (uint32_t)args->data[1].of.i32;
            auto length = (uint32_t)args->data[2].of.i32;
            results->data[0].kind = WASM_I64;
            results->data[0].of.i64 = (int64_t)interface->beiGetAssetBalance(addressOffset, assetnameOffset, length);
            return NULL;
        }
        own wasm_trap_t *beiGetNotFungibleAssetIDs(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto addressOffset = (uint32_t)args->data[0].of.i32;
            auto assetnameOffset = (uint32_t)args->data[1].of.i32;
            auto length = (uint32_t)args->data[2].of.i32;
            auto resultOffset = (uint32_t)args->data[3].of.i32;
            auto resultLength = (uint32_t)args->data[4].of.i32;
            results->data[0].kind = WASM_I32;
            results->data[0].of.i32 = (int32_t)interface->beiGetNotFungibleAssetIDs(addressOffset, assetnameOffset, length, resultOffset, resultLength);
            return NULL;
        }
        own wasm_trap_t *beiGetNotFungibleAssetInfo(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto addressOffset = (uint32_t)args->data[0].of.i32;
            auto assetnameOffset = (uint32_t)args->data[1].of.i32;
            auto length = (uint32_t)args->data[2].of.i32;
            auto assetID = (uint64_t)args->data[3].of.i64;
            auto resultOffset = (uint32_t)args->data[4].of.i32;
            auto resultLength = (uint32_t)args->data[5].of.i32;
            results->data[0].kind = WASM_I32;
            results->data[0].of.i32 = (int32_t)interface->beiGetNotFungibleAssetInfo(addressOffset, assetnameOffset, length, assetID, resultOffset, resultLength);
            return NULL;
        }
#if HERA_DEBUGGING
        own wasm_trap_t *print32(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto value = (uint32_t)args->data[0].of.i32;
            HERA_DEBUG << "DEBUG print32: " << value << " " << hex << "0x" << value << dec << endl;
            return NULL;
        }
        own wasm_trap_t *print64(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto value = (uint64_t)args->data[0].of.i64;
            HERA_DEBUG << "DEBUG print64: " << value << " " << hex << "0x" << value << dec << endl;
            return NULL;
        }
        own wasm_trap_t *printMem(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto offset = (uint32_t)args->data[0].of.i32;
            auto size = (uint32_t)args->data[1].of.i32;
            interface->debugPrintMem(false, offset, size);
            return NULL;
        }
        own wasm_trap_t *printMemHex(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto offset = (uint32_t)args->data[0].of.i32;
            auto size = (uint32_t)args->data[1].of.i32;
            interface->debugPrintMem(true, offset, size);
            return NULL;
        }
        own wasm_trap_t *printStorage(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto offset = (uint32_t)args->data[0].of.i32;
            interface->debugPrintStorage(false, offset);
            return NULL;
        }
        own wasm_trap_t *printStorageHex(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *results)
        {
            auto interface = getInterfaceFromEnv(env);
            auto offset = (uint32_t)args->data[0].of.i32;
            interface->debugPrintStorage(true, offset);
            return NULL;
        }
#endif
        map<string, map<string, ImportFunction> > initImportes()
        {
            map<string, map<string, ImportFunction> > imports;
            // etherum
            const string EthereumModuleName("ethereum");
            auto &ethereumModule = imports[EthereumModuleName];
            {
                auto functype_i64_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_1_0(wasm_valtype_new_i64()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["useGas"] = ImportFunction{functype_i64_0, beiUseGas};
            }
            {
                auto functype_0_i64 = shared_ptr<wasm_functype_t>(wasm_functype_new_0_1(wasm_valtype_new_i64()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["getGasLeft"] = ImportFunction{functype_0_i64, eeiGetGasLeft};
            }
            {
                auto functype_i32_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_1_0(wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["getAddress"] = ImportFunction{functype_i32_0, eeiGetAddress};
            }
            {
                auto functype_i32_2_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_2_0(wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["getExternalBalance"] = ImportFunction{functype_i32_2_0, eeiGetExternalBalance};
            }
            {
                auto functype_i64_i32_i32 = shared_ptr<wasm_functype_t>(wasm_functype_new_2_1(wasm_valtype_new_i64(), wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["getBlockHash"] = ImportFunction{functype_i64_i32_i32, eeiGetBlockHash};
            }
            {
                auto functype_0_i32 = shared_ptr<wasm_functype_t>(wasm_functype_new_0_1(wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["getCallDataSize"] = ImportFunction{functype_0_i32, eeiGetCallDataSize};
            }
            {

                auto functype_i32_3_i32 = shared_ptr<wasm_functype_t>(wasm_functype_new_3_1(wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["callDataCopy"] = ImportFunction{functype_i32_3_i32, eeiCallDataCopy};
            }
            {
                auto functype_i32_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_1_0(wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["getCaller"] = ImportFunction{functype_i32_0, eeiGetCaller};
            }
            {
                auto functype_i32_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_1_0(wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["getCallValue"] = ImportFunction{functype_i32_0, eeiGetCallValue};
            }
            {
                auto functype_i32_3_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_3_0(wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["codeCopy"] = ImportFunction{functype_i32_3_0, eeiCodeCopy};
            }
            {
                auto functype_0_i32 = shared_ptr<wasm_functype_t>(wasm_functype_new_0_1(wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["getCodeSize"] = ImportFunction{functype_0_i32, eeiGetCodeSize};
            }
            {
                auto functype_i32_4_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_4_0(wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["externalCodeCopy"] = ImportFunction{functype_i32_4_0, eeiExternalCodeCopy};
            }
            {
                auto functype_i32_i32 = shared_ptr<wasm_functype_t>(wasm_functype_new_1_1(wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["getExternalCodeSize"] = ImportFunction{functype_i32_i32, eeiGetExternalCodeSize};
            }
            {
                auto functype_i32_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_1_0(wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["getBlockCoinbase"] = ImportFunction{functype_i32_0, eeiGetBlockCoinbase};
            }
            {
                auto functype_i32_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_1_0(wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["getBlockDifficulty"] = ImportFunction{functype_i32_0, eeiGetBlockDifficulty};
            }
            {
                auto functype_0_i64 = shared_ptr<wasm_functype_t>(wasm_functype_new_0_1(wasm_valtype_new_i64()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["getBlockGasLimit"] = ImportFunction{functype_0_i64, eeiGetBlockGasLimit};
            }
            {
                auto functype_i32_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_1_0(wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["getTxGasPrice"] = ImportFunction{functype_i32_0, eeiGetTxGasPrice};
            }
            {
                auto functype_i32_7_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_7_0(wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["log"] = ImportFunction{functype_i32_7_0, eeiLog};
            }
            {
                auto functype_0_i64 = shared_ptr<wasm_functype_t>(wasm_functype_new_0_1(wasm_valtype_new_i64()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["getBlockNumber"] = ImportFunction{functype_0_i64, eeiGetBlockNumber};
            }
            {
                auto functype_0_i64 = shared_ptr<wasm_functype_t>(wasm_functype_new_0_1(wasm_valtype_new_i64()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["getBlockTimestamp"] = ImportFunction{functype_0_i64, eeiGetBlockTimestamp};
            }
            {
                auto functype_i32_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_1_0(wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["getTxOrigin"] = ImportFunction{functype_i32_0, eeiGetTxOrigin};
            }
            {
                auto functype_i32_2_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_2_0(wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["storageStore"] = ImportFunction{functype_i32_2_0, eeiStorageStore};
            }
            {
                auto functype_i32_2_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_2_0(wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["storageLoad"] = ImportFunction{functype_i32_2_0, eeiStorageLoad};
            }
            {
                auto functype_i32_2_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_2_0(wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["finish"] = ImportFunction{functype_i32_2_0, eeiFinish};
            }
            {
                auto functype_i32_2_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_2_0(wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["revert"] = ImportFunction{functype_i32_2_0, eeiRevert};
            }
            {
                auto functype_0_i32 = shared_ptr<wasm_functype_t>(wasm_functype_new_0_1(wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["getReturnDataSize"] = ImportFunction{functype_0_i32, eeiGetReturnDataSize};
            }
            {
                auto functype_i32_3_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_3_0(wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["returnDataCopy"] = ImportFunction{functype_i32_3_0, eeiReturnDataCopy};
            }
            {
                auto functype_i32_4_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_4_0(wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["create"] = ImportFunction{functype_i32_4_0, eeiCreate};
            }
            {
                auto functype_i32_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_1_0(wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["selfDestruct"] = ImportFunction{functype_i32_0, eeiSelfDestruct};
            }
            // bcos environment interface
            auto &bcosModule = imports[BCOS_MODULE_NAME];
            {
                auto functype_i64_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_1_0(wasm_valtype_new_i64()), [](auto p) {
                    wasm_functype_delete(p);
                });
                bcosModule["useGas"] = ImportFunction{functype_i64_0, beiUseGas};
            }
            {
                auto functype_i32_2_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_2_0(wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                bcosModule["finish"] = ImportFunction{functype_i32_2_0, eeiFinish};
            }
            {
                auto functype_i32_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_1_0(wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                bcosModule["getAddress"] = ImportFunction{functype_i32_0, eeiGetAddress};
            }
            {
                auto functype_0_i32 = shared_ptr<wasm_functype_t>(wasm_functype_new_0_1(wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                bcosModule["getCallDataSize"] = ImportFunction{functype_0_i32, eeiGetCallDataSize};
            }
            {
                auto functype_i32_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_1_0(wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                bcosModule["getCallData"] = ImportFunction{functype_i32_0, beiGetCallData};
            }
            {
                auto functype_i32_4_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_4_0(wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                bcosModule["create"] = ImportFunction{functype_i32_4_0, eeiCreate};
            }
            {
                auto functype_i32_4_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_4_0(wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                bcosModule["setStorage"] = ImportFunction{functype_i32_4_0, beiSetStorage};
            }
            {
                auto functype_i32_3_1 = shared_ptr<wasm_functype_t>(wasm_functype_new_3_1(wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                bcosModule["getStorage"] = ImportFunction{functype_i32_3_1, beiGetStorage};
            }
            {
                auto functype_i32_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_1_0(wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                bcosModule["getCaller"] = ImportFunction{functype_i32_0, eeiGetCaller};
            }
            {
                auto functype_i32_2_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_2_0(wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                bcosModule["revert"] = ImportFunction{functype_i32_2_0, eeiRevert};
            }
            {
                auto functype_i32_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_1_0(wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                bcosModule["getTxOrigin"] = ImportFunction{functype_i32_0, eeiGetTxOrigin};
            }
            {
                auto functype_i32_i32 = shared_ptr<wasm_functype_t>(wasm_functype_new_1_1(wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                bcosModule["getExternalCodeSize"] = ImportFunction{functype_i32_i32, eeiGetExternalCodeSize};
            }
            {
                auto functype_0_i64 = shared_ptr<wasm_functype_t>(wasm_functype_new_0_1(wasm_valtype_new_i64()), [](auto p) {
                    wasm_functype_delete(p);
                });
                bcosModule["getBlockNumber"] = ImportFunction{functype_0_i64, eeiGetBlockNumber};
            }
            {
                auto functype_0_i64 = shared_ptr<wasm_functype_t>(wasm_functype_new_0_1(wasm_valtype_new_i64()), [](auto p) {
                    wasm_functype_delete(p);
                });
                bcosModule["getBlockTimestamp"] = ImportFunction{functype_0_i64, eeiGetBlockTimestamp};
            }
            {
                auto functype_i32_7_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_7_0(wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                bcosModule["log"] = ImportFunction{functype_i32_7_0, eeiLog};
            }
            {
                auto functype_0_i32 = shared_ptr<wasm_functype_t>(wasm_functype_new_0_1(wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                bcosModule["getReturnDataSize"] = ImportFunction{functype_0_i32, eeiGetReturnDataSize};
            }
            {
                auto functype_i32_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_1_0(wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                bcosModule["getReturnData"] = ImportFunction{functype_i32_0, beiReturnDataCopy};
            }
            {
                auto functype_i32_3_1 = shared_ptr<wasm_functype_t>(wasm_functype_new_3_1(wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                bcosModule["call"] = ImportFunction{functype_i32_3_1, beiCall};
            }
            // asset interfaces
            {
                auto functype_i32_4_i64_i32_2_1 = shared_ptr<wasm_functype_t>(wasm_functype_new_7_1(wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i64(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                bcosModule["registerAsset"] = ImportFunction{functype_i32_4_i64_i32_2_1, beiRegisterAsset};
            }
            {
                auto functype_i32_3_i64_1_1 = shared_ptr<wasm_functype_t>(wasm_functype_new_4_1(wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i64(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                bcosModule["issueFungibleAsset"] = ImportFunction{functype_i32_3_i64_1_1, beiIssueFungibleAsset};
            }
            {
                auto functype_i32_5_i64_1 = shared_ptr<wasm_functype_t>(wasm_functype_new_5_1(wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i64()), [](auto p) {
                    wasm_functype_delete(p);
                });
                bcosModule["issueNotFungibleAsset"] = ImportFunction{functype_i32_5_i64_1, beiIssueNotFungibleAsset};
            }
            {
                auto functype_i32_3_i64_i32_1_1 = shared_ptr<wasm_functype_t>(wasm_functype_new_5_1(wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i64(), wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                bcosModule["transferAsset"] = ImportFunction{functype_i32_3_i64_i32_1_1, beiTransferAsset};
            }
            {
                auto functype_i32_3_i64_1 = shared_ptr<wasm_functype_t>(wasm_functype_new_3_1(wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i64()), [](auto p) {
                    wasm_functype_delete(p);
                });
                bcosModule["getAssetBalance"] = ImportFunction{functype_i32_3_i64_1, beiGetAssetBalance};
            }
            {
                auto functype_i32_5_i32_1 = shared_ptr<wasm_functype_t>(wasm_functype_new_5_1(wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                bcosModule["getNotFungibleAssetIDs"] = ImportFunction{functype_i32_5_i32_1, beiGetNotFungibleAssetIDs};
            }
            {
                auto functype_i32_3_i64_i32_2_i32_1 = shared_ptr<wasm_functype_t>(wasm_functype_new_6_1(wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i64(), wasm_valtype_new_i32(), wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                bcosModule["getNotFungibleAssetInfo"] = ImportFunction{functype_i32_3_i64_i32_2_i32_1, beiGetNotFungibleAssetInfo};
            }
#if HERA_DEBUGGING
            // debug environment interface
            const string debugModuleName("debug");
            auto &debugModule = imports[debugModuleName];
            {
                auto functype_i32_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_1_0(wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                debugModule["print32"] = ImportFunction{functype_i32_0, print32};
            }
            {
                auto functype_i64_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_1_0(wasm_valtype_new_i64()), [](auto p) {
                    wasm_functype_delete(p);
                });
                debugModule["print64"] = ImportFunction{functype_i64_0, print64};
            }
            {
                auto functype_i32_2_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_2_0(wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["printMem"] = ImportFunction{functype_i32_2_0, printMem};
            }
            {
                auto functype_i32_2_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_2_0(wasm_valtype_new_i32(), wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                ethereumModule["printMemHex"] = ImportFunction{functype_i32_2_0, printMemHex};
            }
            {
                auto functype_i32_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_1_0(wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                debugModule["printStorage"] = ImportFunction{functype_i32_0, printStorage};
            }
            {
                auto functype_i32_0 = shared_ptr<wasm_functype_t>(wasm_functype_new_1_0(wasm_valtype_new_i32()), [](auto p) {
                    wasm_functype_delete(p);
                });
                debugModule["printStorageHex"] = ImportFunction{functype_i32_0, printStorageHex};
            }
#endif

            return imports;
        }
        static map<string, map<string, ImportFunction> > GLOBAL_IMPORTS = initImportes();
        static std::shared_mutex GLOBAL_MODULES_MUTEX_;
        static map<string, shared_ptr<WasmInstanceContainer> > GLOBAL_MODULES_CACHE;
    } // namespace
    static const set<string> eeiFunctions{"useGas", "getGasLeft", "getAddress", "getExternalBalance",
                                          "getBlockHash", "getCallDataSize", "callDataCopy", "getCaller", "getCallValue", "codeCopy",
                                          "getCodeSize", "externalCodeCopy", "getExternalCodeSize", "getBlockCoinbase",
                                          "getBlockDifficulty", "getBlockGasLimit", "getTxGasPrice", "log", "getBlockNumber",
                                          "getBlockTimestamp", "getTxOrigin", "storageStore", "storageLoad", "finish", "revert",
                                          "getReturnDataSize", "returnDataCopy", "call", "callCode", "callDelegate", "callStatic",
                                          "create", "selfDestruct"};
    static const set<string> beiFunctions{"useGas", "finish", "getAddress", "getCallDataSize", "getCallData", "setStorage", "getStorage",
                                          "getCaller", "revert", "getTxOrigin", "getExternalCodeSize", "log", "getReturnDataSize", "getReturnData", "call",
                                          "registerAsset", "issueFungibleAsset", "issueNotFungibleAsset", "transferAsset", "getAssetBalance", "getNotFungibleAssetIDs",
                                          "getNotFungibleAssetInfo"};
    void WasmcEngine::verifyContract(bytes_view code)
    {
        wasm_engine_t *engine = wasm_engine_new();
        wasm_store_t *store = wasm_store_new(engine);
        wasm_byte_vec_t wasm_bytes{code.size(), (char *)code.data()};
        wasm_module_t *module = wasm_module_new(store, &wasm_bytes);
        if (!module)
        {
            wasm_store_delete(store);
            wasm_engine_delete(engine);
            ensureCondition(false, ContractValidationFailure, string("Compile wasm failed"));
        }
        wasm_exporttype_vec_t exportTypes;
        wasm_module_exports(module, &exportTypes);

        // verify exports
        int BCIExportes = 0;
        for (size_t i = 0; i < exportTypes.size; ++i)
        {
            auto name = wasm_exporttype_name(exportTypes.data[i]);
            auto kind = wasm_externtype_kind(wasm_exporttype_type(exportTypes.data[i]));

            if (strncmp("memory", name->data, strlen("memory")) == 0)
            { // multiple memories are not supported for wasmer 0.17.0
                BCIExportes++;
                if (kind != wasm_externkind_enum::WASM_EXTERN_MEMORY)
                {
                    wasm_exporttype_vec_delete(&exportTypes);
                    wasm_module_delete(module);
                    wasm_store_delete(store);
                    wasm_engine_delete(engine);
                    ensureCondition(false, ContractValidationFailure, "\"memory\" is not pointing to memory.");
                }
            }
            else if (strncmp("deploy", name->data, strlen("deploy")) == 0 || strncmp("main", name->data, strlen("main")) == 0 || strncmp("hash_type", name->data, strlen("hash_type")) == 0)
            {
                BCIExportes++;
                if (kind != wasm_externkind_enum::WASM_EXTERN_FUNC)
                {
                    wasm_exporttype_vec_delete(&exportTypes);
                    wasm_module_delete(module);
                    wasm_store_delete(store);
                    wasm_engine_delete(engine);
                    ensureCondition(false, ContractValidationFailure, "\"main\" is not pointing to function.");
                }
            }
            else if (strncmp("__data_end", name->data, strlen("__data_end")) == 0 || strncmp("__heap_base", name->data, strlen("__heap_base")) == 0)
            {
                if (kind != wasm_externkind_enum::WASM_EXTERN_GLOBAL)
                {
                    wasm_exporttype_vec_delete(&exportTypes);
                    wasm_module_delete(module);
                    wasm_store_delete(store);
                    wasm_engine_delete(engine);
                    ensureCondition(false, ContractValidationFailure, "__data_end/__heap_base is not pointing to global.");
                }
            }
            else
            {
                HERA_DEBUG << "Invalid export is " << name->data << "\n";
                wasm_exporttype_vec_delete(&exportTypes);
                wasm_module_delete(module);
                wasm_store_delete(store);
                wasm_engine_delete(engine);
                ensureCondition(false, ContractValidationFailure, "Invalid export is present.");
            }
        }
        if (BCIExportes != 4)
        {
            wasm_exporttype_vec_delete(&exportTypes);
            wasm_module_delete(module);
            wasm_store_delete(store);
            wasm_engine_delete(engine);
            ensureCondition(false, ContractValidationFailure, "BCI(deploy/main/hash_type/memory) are not all exported.");
        }
        wasm_exporttype_vec_delete(&exportTypes);

        // verify imports
        wasm_importtype_vec_t importTypes;
        wasm_module_imports(module, &importTypes);
        for (size_t i = 0; i < importTypes.size; ++i)
        {
            auto moduleName = wasm_importtype_module(importTypes.data[i]);
            auto kind = wasm_externtype_kind(wasm_importtype_type(importTypes.data[i]));
            auto objectName = wasm_importtype_name(importTypes.data[i]);

#if HERA_DEBUGGING
            if (strncmp("debug", moduleName->data, strlen("debug")) == 0)
                continue;
#endif
            if (strncmp("bcos", moduleName->data, strlen("bcos")) != 0 && strncmp("ethereum", moduleName->data, strlen("ethereum")) != 0)
            {
                wasm_importtype_vec_delete(&importTypes);
                wasm_module_delete(module);
                wasm_store_delete(store);
                wasm_engine_delete(engine);
                ensureCondition(false, ContractValidationFailure, "Import from invalid namespace.");
            }
            auto objectNameString = string(objectName->data, objectName->size);
            if (!beiFunctions.count(objectNameString) && !eeiFunctions.count(objectNameString))
            {
                wasm_importtype_vec_delete(&importTypes);
                wasm_module_delete(module);
                wasm_store_delete(store);
                wasm_engine_delete(engine);
                ensureCondition(false, ContractValidationFailure, "Importing invalid EEI method " + objectNameString);
            }
            if (kind != wasm_externkind_enum::WASM_EXTERN_FUNC)
            {
                wasm_importtype_vec_delete(&importTypes);
                wasm_module_delete(module);
                wasm_store_delete(store);
                wasm_engine_delete(engine);
                ensureCondition(false, ContractValidationFailure, "Imported function type mismatch.");
            }
        }
        wasm_importtype_vec_delete(&importTypes);
        wasm_module_delete(module);
        wasm_store_delete(store);
        wasm_engine_delete(engine);
    }

    wasm_extern_t *findExternByName(const string &targetName, const wasm_extern_vec_t &exports, const wasm_exporttype_vec_t &exportTypes)
    {
        int index = -1;
        for (size_t i = 0; i < exportTypes.size; ++i)
        {
            auto name = wasm_exporttype_name(exportTypes.data[i]);
            string objectName((char *)name->data, name->size);
            // HERA_DEBUG << "exports have " << objectName << "\n";
            if (objectName == targetName)
            {
                index = (int)i;
            }
        }
        if (index >= 0)
        {
            return exports.data[index];
        }

        return NULL;
    }
    void print_frame(wasm_frame_t *frame)
    {
        printf("> %p @ 0x%zx = %" PRIu32 ".0x%zx\n",
               wasm_frame_instance(frame),
               wasm_frame_module_offset(frame),
               wasm_frame_func_index(frame),
               wasm_frame_func_offset(frame));
    }
    string processTrap(wasm_trap_t *trap)
    {
        own wasm_name_t message;
        wasm_trap_message(trap, &message);
        string ret(message.data, message.size);
        wasm_name_delete(&message);
#if HERA_DEBUGGING && HERA_WASMER
        HERA_DEBUG << "Printing origin...\n";
        own wasm_frame_t *frame = wasm_trap_origin(trap);
        if (frame)
        {
            print_frame(frame);
            wasm_frame_delete(frame);
        }
        else
        {
            printf("> Empty origin.\n");
        }
        HERA_DEBUG << "Printing trace...\n";
        own wasm_frame_vec_t trace;
        wasm_trap_trace(trap, &trace);
        if (trace.size > 0)
        {
            for (size_t i = 0; i < trace.size; ++i)
            {
                print_frame(trace.data[i]);
            }
        }
        else
        {
            HERA_DEBUG << "> Empty trace.\n";
        }
        wasm_frame_vec_delete(&trace);
#endif
        wasm_trap_delete(trap);
        return ret;
    }

    shared_ptr<WasmInstance> createWasmInstance(wasm_store_t *store, wasm_module_t *module, void *env)
    {
#ifdef PERF_TIME
        auto end = system_clock::now();
#endif
        wasm_importtype_vec_t importTypes;
        wasm_module_imports(module, &importTypes);
        vector<wasm_extern_t *> imports;
        imports.reserve(importTypes.size);
        auto functions = make_shared<vector<shared_ptr<wasm_func_t> > >();
        functions->reserve(importTypes.size);
        for (size_t i = 0; i < importTypes.size; ++i)
        {
            auto moduleName = wasm_importtype_module(importTypes.data[i]);
            // auto kind = wasm_externtype_kind(wasm_importtype_type(importTypes.data[i]));
            auto objectName = wasm_importtype_name(importTypes.data[i]);
            auto functionName = string(objectName->data, objectName->size);

            if (strncmp("bcos", moduleName->data, strlen("bcos")) != 0 && strncmp("ethereum", moduleName->data, strlen("ethereum")) != 0
#if HERA_DEBUGGING
                && strncmp("debug", moduleName->data, strlen("debug")) != 0
#endif
            )
            {
                wasm_importtype_vec_delete(&importTypes);
                ensureCondition(false, ContractValidationFailure, "Import from invalid namespace.");
                break;
            }

            auto &hostFunctions = GLOBAL_IMPORTS[string(moduleName->data, moduleName->size)];

            if (hostFunctions.count(functionName))
            {
                wasm_func_t *host_func = wasm_func_new_with_env(store, hostFunctions[functionName].functionType.get(), hostFunctions[functionName].function, env, NULL);
                if (!host_func)
                {
                    functions.reset();
                    wasm_importtype_vec_delete(&importTypes);
                    ensureCondition(false, ContractValidationFailure, functionName + " import failed.");
                }
                imports.push_back(wasm_func_as_extern(host_func));
                functions->push_back(shared_ptr<wasm_func_t>(host_func, [](auto p) { wasm_func_delete(p); }));
                // HERA_DEBUG << i << " " << string(moduleName->data, moduleName->size) << "::" << functionName << " imported\n";
            }
            else
            {
                functions.reset();
                wasm_importtype_vec_delete(&importTypes);
                ensureCondition(false, ContractValidationFailure, functionName + " is not a supported function");
            }
        }
        wasm_importtype_vec_delete(&importTypes);
#ifdef PERF_TIME
        auto end2 = system_clock::now();
        cout << "wasm new imports used(us) : " << duration_cast<microseconds>(end2 - end).count() << endl;
#endif
        HERA_DEBUG << "Create wasm instance...\n";
        wasm_extern_vec_t import_object{imports.size(), imports.data()};
        // Assert the Wasm instantion completed
        own wasm_trap_t *trap = NULL;
        wasm_instance_t *instance = NULL;
#if HERA_WASMER
        instance = wasm_instance_new(store, module, &import_object, &trap);
#else
        wasmtime_error_t *error = wasmtime_instance_new(store, module, &import_object, &instance, &trap);
#endif
        if (!instance)
        {
            string message;
            if (trap)
            {
                message = processTrap(trap);
            }
            else
            {
#if HERA_WASMER
                message = get_last_wasmer_error();
#else
                message = get_wasmtime_error("failed to instantiate", error, trap);
#endif
            }
            HERA_DEBUG << "Create wasm instance failed, " << message << "...\n";
            ensureCondition(false, ContractValidationFailure, "Error instantiating wasm");
        }
        auto wasmInstance = shared_ptr<WasmInstance>(new WasmInstance(functions, instance),[](auto p){
#if HERA_WASMER
            if (p->memory)
            {
                wasm_memory_delete(p->memory);
                p->memory = nullptr;
            }
            if (p->hashTypeFunc)
            {
                wasm_func_delete(p->hashTypeFunc);
                p->hashTypeFunc = nullptr;
            }
            if (p->deployFunc)
            {
                wasm_func_delete(p->deployFunc);
                p->deployFunc = nullptr;
            }
            if (p->mainFunc)
            {
                wasm_func_delete(p->mainFunc);
                p->mainFunc = nullptr;
            }
#endif
            wasm_extern_vec_delete(&p->exports);
            wasm_instance_delete(p->instance);
            p->functions.reset();
            delete p;
        });
        wasm_instance_exports(instance, &wasmInstance->exports);
        wasm_exporttype_vec_t exportTypes;
        wasm_module_exports(module, &exportTypes);
        // get memory TODO: remove findExternByName
        auto memoryExtern = findExternByName("memory", wasmInstance->exports, exportTypes);
        if (!memoryExtern)
        {
            ensureCondition(false, InvalidMemoryAccess, string("get memory from wasm failed"));
        }
        auto memory = wasm_extern_as_memory(memoryExtern);
        HERA_DEBUG << "wasm memory pages is " << wasm_memory_size(memory) << "\n";
        if (wasm_memory_size(memory) == 0)
        {
#if HERA_WASMER
            wasm_memory_delete(memory);
#endif
            wasm_exporttype_vec_delete(&exportTypes);
            ensureCondition(false, InvalidMemoryAccess, string("wasm memory pages must greater than 1"));
        }
        wasmInstance->memory = memory;
        auto hashTypeFuncExtern = findExternByName("hash_type", wasmInstance->exports, exportTypes);
        if (!hashTypeFuncExtern)
        {
            wasm_exporttype_vec_delete(&exportTypes);
            ensureCondition(false, ContractValidationFailure, "get hash function failed");
        }
        wasmInstance->hashTypeFunc = wasm_extern_as_func(hashTypeFuncExtern);
        auto deployFuncExtern = findExternByName("deploy", wasmInstance->exports, exportTypes);
        if (!deployFuncExtern)
        {
            wasm_exporttype_vec_delete(&exportTypes);
            ensureCondition(false, ContractValidationFailure, "can't find deploy");
        }
        wasmInstance->deployFunc = wasm_extern_as_func(deployFuncExtern);
        auto mainFuncExtern = findExternByName("main", wasmInstance->exports, exportTypes);
        if (!mainFuncExtern)
        {
            wasm_exporttype_vec_delete(&exportTypes);
            ensureCondition(false, ContractValidationFailure, "can't main deploy");
        }
        wasmInstance->mainFunc = wasm_extern_as_func(mainFuncExtern);
        return wasmInstance;
    }

    shared_ptr<WasmInstanceContainer> getWasmInstanceContainer(const string &address, bytes_view code, void *env)
    {
#ifdef PERF_TIME
        auto end = system_clock::now();
#endif
        {
            std::shared_lock lock(GLOBAL_MODULES_MUTEX_);
            if (GLOBAL_MODULES_CACHE.count(address))
            {
                return GLOBAL_MODULES_CACHE[address];
            }
        }

#if HERA_WASMER
        wasm_engine_t *engine = wasm_engine_new();
#else
        wasm_config_t *wasm_config = wasm_config_new();
        wasmtime_config_max_instances_set(wasm_config, MAX_INSTANCE);
        wasm_engine_t *engine = wasm_engine_new_with_config(wasm_config);
#endif
        wasm_store_t *store = wasm_store_new(engine);

        wasm_byte_vec_t wasm_bytes{code.size(), (char *)code.data()};
        auto module = wasm_module_new(store, &wasm_bytes);
        if (!module)
        {
            wasm_store_delete(store);
            wasm_engine_delete(engine);
            ensureCondition(false, ContractValidationFailure, string("Compile wasm failed"));
        }
        // #if HERA_WASMER
        //         auto store = wasmModule->store;
        // #else
        //         auto store = wasm_store_new(wasmModule->engine);
        //         auto store_holder = shared_ptr<wasm_store_t>(store, [](auto p) { wasm_store_delete(p); });
        // #endif
        shared_ptr<WasmInstance> wasmInstance = createWasmInstance(store, module, env);
        auto p = shared_ptr<WasmInstanceContainer>(new WasmInstanceContainer{engine, store, module, {wasmInstance}}, [](auto p) {
            if (p->module)
            {
                wasm_module_delete(p->module);
                p->module = nullptr;
            }
            if (p->store)
            {
                wasm_store_delete(p->store);
                p->store = nullptr;
            }
            if (p->engine)
            {
                wasm_engine_delete(p->engine);
                p->engine = nullptr;
            }
            delete p;
        });

#ifdef PERF_TIME
        auto end2 = system_clock::now();
        cout << "wasm instance used(us)    : " << duration_cast<microseconds>(end2 - end).count() << endl;
#endif
        std::unique_lock lock(GLOBAL_MODULES_MUTEX_);
        GLOBAL_MODULES_CACHE[address] = p;
        return p;
    }
    shared_ptr<WasmInstance> getInstanceFromContainer(shared_ptr<WasmInstanceContainer> container, void *env)
    {
        {
            std::shared_lock lock(container->instancesMutex);
            for (auto i : container->instances)
            {
                if (i->idle.load())
                {
                    if (i->idle.exchange(false))
                    {
                        return i;
                    }
                }
            }
        }
        shared_ptr<WasmInstance> wasmInstance = createWasmInstance(container->store, container->module, env);
        wasmInstance->idle.store(false);
        std::unique_lock lock(container->instancesMutex);
        container->instances.push_back(wasmInstance);
        return wasmInstance;
    }
    ExecutionResult WasmcEngine::execute(evmc::HostContext &context, bytes_view code,
                                         bytes_view state_code, evmc_message const &msg, bool meterInterfaceGas)
    {
        instantiationStarted();
        HERA_DEBUG << "Executing use wasmc API...\n";
        // Set up interface to eei host functions
        ExecutionResult result;
        WasmcInterface interface{context, state_code, msg, result, meterInterfaceGas};

        // Instantiate a WebAssembly Instance from Wasm bytes and imports
        // TODO: check if need free code, for me it seems wasmer will free
        HERA_DEBUG << "Compile wasm code use wasmc API...\n";
#ifdef PERF_TIME
        auto start = system_clock::now();
#endif
        string myAddress((const char *)msg.destination.bytes, 20);
        shared_ptr<WasmInstanceContainer> containter = getWasmInstanceContainer(myAddress, code, (void *)&interface);
#ifdef PERF_TIME
        auto start1 = system_clock::now();
#endif
        auto wasmInstance = getInstanceFromContainer(containter, (void *)&interface);
        auto instanceHolder = InstanceHolder{wasmInstance};
#ifdef PERF_TIME
        auto end = system_clock::now();
        cout << "wasm new module used(us)  : " << duration_cast<microseconds>(start1 - start).count() << endl
             << "wasm get instance used(us): " << duration_cast<microseconds>(end - start1).count() << endl;
#endif

        interface.setWasmStore(containter->store);
        interface.setWasmMemory(wasmInstance->memory);

        // Call the Wasm function
        const char *callName = "main";
        if (msg.kind == EVMC_CREATE)
        {
            callName = "deploy";
            bool useSM3Hash = 0;
            if (context.get_host_context()->sm3_hash_fn)
            {
                useSM3Hash = 1;
            }
            HERA_DEBUG << "host hash algorithm is " << (useSM3Hash ? "sm3" : "keccak256") << ", Get hash type of contract...\n";

            // call hash_type
            wasm_val_t args_val[] = {};
            wasm_val_t results_val[1] = {WASM_INIT_VAL};
            wasm_val_vec_t args = WASM_ARRAY_VEC(args_val);
            wasm_val_vec_t results = WASM_ARRAY_VEC(results_val);
            auto trap = wasm_func_call(wasmInstance->hashTypeFunc, &args, &results);
            if (trap)
            { //call hash_type failed
                auto message = processTrap(trap);
                ensureCondition(false, ContractValidationFailure, "call hash_type failed, " + message);
            }
            HERA_DEBUG << "Contract hash algorithm is " << (results_val[0].of.i32 ? "sm3\n" : "keccak256\n");

            if (results_val[0].of.i32 != useSM3Hash)
            { // 0:keccak256, 1:sm3
                ensureCondition(false, ContractValidationFailure, "hash type mismatch");
            }
        }
#ifdef PERF_TIME
        auto end4 = system_clock::now();
#endif
        wasm_trap_t *trap = nullptr;
        try
        {
            HERA_DEBUG << "Executing contract " << callName << "...\n";
            auto func = wasmInstance->mainFunc;
            if (callName == string("deploy"))
            {
                func = wasmInstance->deployFunc;
            }
#ifdef PERF_TIME
            end4 = system_clock::now();
            cout << "wasm get exports used(us) : " << duration_cast<microseconds>(end4 - end).count() << endl;
#endif
            // call
            wasm_val_t args_val[] = {};
            wasm_val_t results_val[] = {};
            wasm_val_vec_t args = WASM_ARRAY_VEC(args_val);
            wasm_val_vec_t results = WASM_ARRAY_VEC(results_val);
            trap = wasm_func_call(func, &args, &results);
        }
        catch (EndExecution const &)
        {
            // This exception is ignored here because we consider it to be a success.
            // It is only a clutch for POSIX style exit()
        }
#ifdef PERF_TIME
        auto end5 = system_clock::now();
        cout << "wasm execute main used(us): " << duration_cast<microseconds>(end5 - end4).count() << endl;
#endif
        if (msg.kind == EVMC_CREATE && !result.isRevert)
        {
            result.returnValue = code;
        }
        executionFinished();
#ifdef PERF_TIME
        auto end6 = system_clock::now();
        cout << "wasm free exports used(us): " << duration_cast<microseconds>(end6 - end5).count() << endl;
#endif
        if (trap)
        { //call main/deploy failed, process trap, print frame and trace
            auto errorMessage = processTrap(trap);
            result.isRevert = true;
            HERA_DEBUG << "call " << callName << ", error message: " << errorMessage << "\n";
            // throw specific exception according to error message
            if (errorMessage.find(OUT_OF_GAS) != std::string::npos)
            {
                HERA_DEBUG << OUT_OF_GAS << "\n";
                throw hera::OutOfGas(OUT_OF_GAS);
            }
            else if (errorMessage.find(UNREACHABLE) != std::string::npos)
            {
                HERA_DEBUG << UNREACHABLE << "\n";
                throw hera::Unreachable(UNREACHABLE);
            }
            else if (errorMessage.find(STACK_OVERFLOW) != std::string::npos)
            {
                HERA_DEBUG << STACK_OVERFLOW << "\n";
                throw hera::Unreachable(STACK_OVERFLOW);
            }
            else if (errorMessage.find(REVERT) != std::string::npos)
            {
                HERA_DEBUG << REVERT << "\n";
            }
            else if (errorMessage.find(MEMORY_ACCESS) != std::string::npos)
            {
                throw hera::InvalidMemoryAccess(MEMORY_ACCESS);
            }
            else if (errorMessage.find(FINISH) != std::string::npos)
            {
                result.isRevert = false;
                HERA_DEBUG << FINISH << "\n";
            }
            else
            {
#if HERA_WASMER
                HERA_DEBUG << "Unknown error. " << get_last_wasmer_error() << "\n";
#endif
                throw std::runtime_error("Unknown error.");
            }
        }
#if 0
        else
        { // FIXME: debug log should delete before release
            HERA_DEBUG << "Output size is " << result.returnValue.size() << ", ouput=";
            for (size_t i = 0; i < result.returnValue.size(); ++i)
            {
                HERA_DEBUG << hex << result.returnValue[i];
            }
            HERA_DEBUG << " done\n";
        }
#endif
#ifdef PERF_TIME
        auto end7 = system_clock::now();
        cout << "wasm parse trap used(us)  : " << duration_cast<microseconds>(end7 - end6).count() << ", total = " << duration_cast<microseconds>(end7 - start).count() << endl;
#endif
        return result;
    };
} // namespace hera
