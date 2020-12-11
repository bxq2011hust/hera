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

#include "wasmer.h"
#include "c-api/wasmer.hh"
#include "debugging.h"
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <vector>

using namespace std;

#define hostEnsure(condition, ctx, msg) \
    if (!(condition))                   \
    {                                   \
        HERA_DEBUG << msg << "\n";      \
        wasmer_trap(ctx, msg);          \
    }
const char *OUT_OF_GAS = "Out of gas.";
const char *REVERT = "revert";
const char *UNREACHABLE = "unreachable";
namespace hera
{
    class WasmerEthereumInterface : public EthereumInterface
    {
    public:
        explicit WasmerEthereumInterface(evmc::HostContext &_context, bytes_view _code,
                                         evmc_message const &_msg, ExecutionResult &_result, bool _meterGas)
            : EthereumInterface(_context, _code, _msg, _result, _meterGas)
        {
        }

        void setWasmMemory(const wasmer_memory_t *_wasmMemory) { m_wasmMemory = _wasmMemory; }
        void setWasmContext(const wasmer_instance_context_t *_wasmContext)
        {
            m_wasmContext = const_cast<wasmer_instance_context_t *>(_wasmContext);
        }

        void takeGas(int64_t gas) override
        { // NOTE: gas >= 0 is validated by the callers of this method
            hostEnsure(gas <= m_result.gasLeft, m_wasmContext, OUT_OF_GAS);
            m_result.gasLeft -= gas;
        }
        void beiRevertOrFinish(const wasmer_instance_context_t *_wasmContext, bool revert, uint32_t offset, uint32_t size)
        {
            HERA_DEBUG << (revert ? "revert " : "finish ") << hex << offset << " " << size << dec << "\n";

            ensureSourceMemoryBounds(offset, size);
            m_result.returnValue = bytes(size, '\0');
            loadMemory(offset, m_result.returnValue, size);

            m_result.isRevert = revert;
            if (revert)
            {
                wasmer_trap(_wasmContext, "revert");
                return;
            }
            //throw EndExecution{};
            //FIXME: throw exception will crash host
            int value_one = 24;
            int value_two = 0;
            int error = value_one / value_two;
            (void)error;
        }

    private:
        // These assume that m_wasmMemory was set prior to execution.
        size_t memorySize() const override { return wasmer_memory_data_length(m_wasmMemory); }
        void memorySet(size_t offset, uint8_t value) override
        {
            auto data = wasmer_memory_data(m_wasmMemory);
            ensureCondition(data != NULL, InvalidMemoryAccess, string("memorySet failed"));
            data[offset] = value;
        }
        uint8_t memoryGet(size_t offset) override
        {
            ensureCondition(memorySize() >= offset, InvalidMemoryAccess,
                            "Memory is shorter than requested segment");
            auto data = wasmer_memory_data(m_wasmMemory);
            ensureCondition(data != NULL, InvalidMemoryAccess, string("memoryGet failed"));
            return data[offset];
        }
        uint8_t *memoryPointer(size_t offset, size_t length) override
        {
            ensureCondition(memorySize() >= (offset + length), InvalidMemoryAccess,
                            "Memory is shorter than requested segment");
            auto data = wasmer_memory_data(m_wasmMemory);
            return data + offset;
        }

        const wasmer_memory_t *m_wasmMemory;
        wasmer_instance_context_t *m_wasmContext = nullptr;
    };

    unique_ptr<WasmEngine> WasmerEngine::create()
    {
        return unique_ptr<WasmEngine>{new WasmerEngine};
    }
    namespace
    {
        wasmer_value_tag i32[] = {wasmer_value_tag::WASM_I32};
        wasmer_value_tag i64[] = {wasmer_value_tag::WASM_I64};
        wasmer_value_tag i64_i32[] = {wasmer_value_tag::WASM_I64, wasmer_value_tag::WASM_I32};
        wasmer_value_tag i32_2[] = {wasmer_value_tag::WASM_I32, wasmer_value_tag::WASM_I32};
        wasmer_value_tag i32_3[] = {
            wasmer_value_tag::WASM_I32, wasmer_value_tag::WASM_I32, wasmer_value_tag::WASM_I32};
        wasmer_value_tag i32_4[] = {wasmer_value_tag::WASM_I32, wasmer_value_tag::WASM_I32,
                                    wasmer_value_tag::WASM_I32, wasmer_value_tag::WASM_I32};
        wasmer_value_tag i32_5[] = {wasmer_value_tag::WASM_I32, wasmer_value_tag::WASM_I32,
                                    wasmer_value_tag::WASM_I32, wasmer_value_tag::WASM_I32, wasmer_value_tag::WASM_I32};

        wasmer_value_tag i32_4_i64_i32_2[] = {wasmer_value_tag::WASM_I32, wasmer_value_tag::WASM_I32,
                                              wasmer_value_tag::WASM_I32, wasmer_value_tag::WASM_I32, wasmer_value_tag::WASM_I64,
                                              wasmer_value_tag::WASM_I32, wasmer_value_tag::WASM_I32};
        wasmer_value_tag i32_3_i64[] = {wasmer_value_tag::WASM_I32, wasmer_value_tag::WASM_I32,
                                        wasmer_value_tag::WASM_I32, wasmer_value_tag::WASM_I64};
        wasmer_value_tag i32_3_i64_i32[] = {wasmer_value_tag::WASM_I32, wasmer_value_tag::WASM_I32, wasmer_value_tag::WASM_I32,
                                            wasmer_value_tag::WASM_I64, wasmer_value_tag::WASM_I32};
        wasmer_value_tag i32_7[] = {wasmer_value_tag::WASM_I32, wasmer_value_tag::WASM_I32,
                                    wasmer_value_tag::WASM_I32, wasmer_value_tag::WASM_I32, wasmer_value_tag::WASM_I32,
                                    wasmer_value_tag::WASM_I32, wasmer_value_tag::WASM_I32};

        // Function to print the most recent error string from Wasmer if we have them
        string getWasmerErrorString()
        {
            int error_len = wasmer_last_error_length();
            char *error_str = new char[(uint64_t)error_len];
            wasmer_last_error_message(error_str, error_len);
            string error((const char *)error_str, (size_t)error_len);
            delete[] error_str;
            return error;
        }
#if 0
        // Function to create a wasmer memory instance, so we can import
        // memory into a wasmer instance.
        wasmer_memory_t *create_wasmer_memory()
        {
            // Create our initial size of the memory
            wasmer_memory_t *memory = NULL;
            // Create our maximum memory size.
            // .has_some represents wether or not the memory has a maximum size
            // .some is the value of the maxiumum size
            wasmer_limit_option_t max = {true, 256};
            // Create our memory descriptor, to set our minimum and maximum memory size
            // .min is the minimum size of the memory
            // .max is the maximuum size of the memory
            wasmer_limits_t descriptor = {64, max};

            // Create our memory instance, using our memory and descriptor,
            wasmer_result_t memory_result = wasmer_memory_new(&memory, descriptor);
            // Ensure the memory was instantiated successfully.
            if (memory_result != wasmer_result_t::WASMER_OK)
            {
                ensureCondition(memory != NULL, InvalidMemoryAccess, string("get memory from wasmer failed, ") + getWasmerErrorString());
            }

            // Return the Wasmer Memory Instance
            return memory;
        }
#endif
        wasmer_byte_array getNameArray(const char *name)
        {
            return wasmer_byte_array{(const uint8_t *)name, (uint32_t)strlen(name)};
        }

        WasmerEthereumInterface *getInterfaceFromVontext(wasmer_instance_context_t *ctx)
        {
            return (WasmerEthereumInterface *)wasmer_instance_context_data_get(ctx);
        }

        void beiUseGas(wasmer_instance_context_t *ctx, int64_t gas)
        {
            auto interface = getInterfaceFromVontext(ctx);
            HERA_DEBUG << " useGas " << gas << "\n";
            hostEnsure(gas >= 0, ctx, "Negative gas supplied.");
            interface->takeGas(gas);
        }
        int64_t eeiGetGasLeft(wasmer_instance_context_t *ctx)
        {
            auto interface = getInterfaceFromVontext(ctx);
            return interface->eeiGetGasLeft();
        }
        void eeiGetAddress(wasmer_instance_context_t *ctx, uint32_t resultOffset)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->eeiGetAddress(resultOffset);
        }
        void eeiGetExternalBalance(
            wasmer_instance_context_t *ctx, uint32_t addressOffset, uint32_t resultOffset)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->eeiGetExternalBalance(addressOffset, resultOffset);
        }

        uint32_t eeiGetBlockHash(wasmer_instance_context_t *ctx, uint64_t number, uint32_t resultOffset)
        {
            auto interface = getInterfaceFromVontext(ctx);
            return interface->eeiGetBlockHash(number, resultOffset);
        }
        uint32_t eeiGetCallDataSize(wasmer_instance_context_t *ctx)
        {
            auto interface = getInterfaceFromVontext(ctx);
            return interface->eeiGetCallDataSize();
        }
        void eeiCallDataCopy(
            wasmer_instance_context_t *ctx, uint32_t resultOffset, uint32_t dataOffset, uint32_t length)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->eeiCallDataCopy(resultOffset, dataOffset, length);
        }
        void eeiGetCaller(wasmer_instance_context_t *ctx, uint32_t resultOffset)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->eeiGetCaller(resultOffset);
        }
        void eeiGetCallValue(wasmer_instance_context_t *ctx, uint32_t resultOffset)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->eeiGetCallValue(resultOffset);
        }
        void eeiCodeCopy(
            wasmer_instance_context_t *ctx, uint32_t resultOffset, uint32_t codeOffset, uint32_t length)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->eeiCodeCopy(resultOffset, codeOffset, length);
        }
        uint32_t eeiGetCodeSize(wasmer_instance_context_t *ctx)
        {
            auto interface = getInterfaceFromVontext(ctx);
            return interface->eeiGetCodeSize();
        }
        void eeiExternalCodeCopy(wasmer_instance_context_t *ctx, uint32_t addressOffset,
                                 uint32_t resultOffset, uint32_t codeOffset, uint32_t length)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->eeiExternalCodeCopy(addressOffset, resultOffset, codeOffset, length);
        }
        uint32_t eeiGetExternalCodeSize(wasmer_instance_context_t *ctx, uint32_t addressOffset)
        {
            auto interface = getInterfaceFromVontext(ctx);
            return interface->eeiGetExternalCodeSize(addressOffset);
        }
        void eeiGetBlockCoinbase(wasmer_instance_context_t *ctx, uint32_t resultOffset)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->eeiGetBlockCoinbase(resultOffset);
        }
        void eeiGetBlockDifficulty(wasmer_instance_context_t *ctx, uint32_t offset)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->eeiGetBlockDifficulty(offset);
        }
        int64_t eeiGetBlockGasLimit(wasmer_instance_context_t *ctx)
        {
            auto interface = getInterfaceFromVontext(ctx);
            return interface->eeiGetBlockGasLimit();
        }
        void eeiGetTxGasPrice(wasmer_instance_context_t *ctx, uint32_t valueOffset)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->eeiGetTxGasPrice(valueOffset);
        }
        void eeiLog(wasmer_instance_context_t *ctx, uint32_t dataOffset, uint32_t length,
                    uint32_t numberOfTopics, uint32_t topic1, uint32_t topic2, uint32_t topic3, uint32_t topic4)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->eeiLog(dataOffset, length, numberOfTopics, topic1, topic2, topic3, topic4);
        }
        int64_t eeiGetBlockNumber(wasmer_instance_context_t *ctx)
        {
            auto interface = getInterfaceFromVontext(ctx);
            return interface->eeiGetBlockNumber();
        }
        int64_t eeiGetBlockTimestamp(wasmer_instance_context_t *ctx)
        {
            auto interface = getInterfaceFromVontext(ctx);
            return interface->eeiGetBlockTimestamp();
        }
        void eeiGetTxOrigin(wasmer_instance_context_t *ctx, uint32_t resultOffset)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->eeiGetTxOrigin(resultOffset);
        }
        void eeiStorageStore(wasmer_instance_context_t *ctx, uint32_t pathOffset, uint32_t valueOffset)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->eeiStorageStore(pathOffset, valueOffset);
        }
        void eeiStorageLoad(wasmer_instance_context_t *ctx, uint32_t pathOffset, uint32_t resultOffset)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->eeiStorageLoad(pathOffset, resultOffset);
        }
        void beiSetStorage(wasmer_instance_context_t *ctx, uint32_t keyOffset, uint32_t keyLength,
                           uint32_t valueOffset, uint32_t valueLength)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->beiSetStorage(keyOffset, keyLength, valueOffset, valueLength);
        }
        int32_t beiGetStorage(
            wasmer_instance_context_t *ctx, uint32_t keyOffset, uint32_t keyLength, uint32_t valueOffset)
        {
            auto interface = getInterfaceFromVontext(ctx);
            const int32_t maxLength = 19264;
            return interface->beiGetStorage(keyOffset, keyLength, valueOffset, maxLength);
        }
        void beiGetCallData(wasmer_instance_context_t *ctx, uint32_t resultOffset)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->eeiCallDataCopy(resultOffset, 0, interface->eeiGetCallDataSize());
        }
        void eeiFinish(wasmer_instance_context_t *ctx, uint32_t offset, uint32_t size)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->beiRevertOrFinish(ctx, false, offset, size);
        }
        void eeiRevert(wasmer_instance_context_t *ctx, uint32_t offset, uint32_t size)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->beiRevertOrFinish(ctx, true, offset, size);
        }
        void beiCall(wasmer_instance_context_t *ctx, uint32_t addressOffset, uint32_t dataOffset, uint32_t dataLength)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->eeiCall(EthereumInterface::EEICallKind::Call, interface->eeiGetGasLeft(), addressOffset, 0, dataOffset, dataLength);
        }
        uint32_t eeiGetReturnDataSize(wasmer_instance_context_t *ctx)
        {
            auto interface = getInterfaceFromVontext(ctx);
            return interface->eeiGetReturnDataSize();
        }
        void eeiReturnDataCopy(
            wasmer_instance_context_t *ctx, uint32_t dataOffset, uint32_t offset, uint32_t size)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->eeiReturnDataCopy(dataOffset, offset, size);
        }
        void beiReturnDataCopy(
            wasmer_instance_context_t *ctx, uint32_t dataOffset)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->eeiReturnDataCopy(dataOffset, 0, interface->eeiGetReturnDataSize());
        }
        uint32_t eeiCreate(wasmer_instance_context_t *ctx, uint32_t valueOffset, uint32_t dataOffset,
                           uint32_t length, uint32_t resultOffset)
        {
            auto interface = getInterfaceFromVontext(ctx);
            return interface->eeiCreate(valueOffset, dataOffset, length, resultOffset);
        }
        void eeiSelfDestruct(wasmer_instance_context_t *ctx, uint32_t addressOffset)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->eeiSelfDestruct(addressOffset);
        }
        int32_t beiRegisterAsset(wasmer_instance_context_t *ctx, uint32_t assetnameOffset, uint32_t length, uint32_t addressOffset, int32_t fungible, uint64_t total, uint32_t descriptionOffset, uint32_t descriptionLength)
        {
            auto interface = getInterfaceFromVontext(ctx);
            return interface->beiRegisterAsset(assetnameOffset, length, addressOffset, fungible, total, descriptionOffset, descriptionLength);
        }
        int32_t beiIssueFungibleAsset(wasmer_instance_context_t *ctx, uint32_t addressOffset, uint32_t assetnameOffset, uint32_t length, uint64_t amount)
        {
            auto interface = getInterfaceFromVontext(ctx);
            return interface->beiIssueFungibleAsset(addressOffset, assetnameOffset, length, amount);
        }
        uint64_t beiIssueNotFungibleAsset(wasmer_instance_context_t *ctx, uint32_t addressOffset, uint32_t assetnameOffset, uint32_t length, uint32_t uriOffset, uint32_t uriLength)
        {
            auto interface = getInterfaceFromVontext(ctx);
            return interface->beiIssueNotFungibleAsset(addressOffset, assetnameOffset, length, uriOffset, uriLength);
        }
        int32_t beiTransferAsset(wasmer_instance_context_t *ctx, uint32_t addressOffset, uint32_t assetnameOffset, uint32_t length, uint64_t amountOrID, int32_t fromSelf)
        {
            auto interface = getInterfaceFromVontext(ctx);
            return interface->beiTransferAsset(addressOffset, assetnameOffset, length, amountOrID, fromSelf);
        }
        uint64_t beiGetAssetBanlance(wasmer_instance_context_t *ctx, uint32_t addressOffset, uint32_t assetnameOffset, uint32_t length)
        {
            auto interface = getInterfaceFromVontext(ctx);
            return interface->beiGetAssetBanlance(addressOffset, assetnameOffset, length);
        }
        int32_t beiGetNotFungibleAssetIDs(wasmer_instance_context_t *ctx, uint32_t addressOffset, uint32_t assetnameOffset, uint32_t length, uint32_t resultOffset, uint32_t resultLength)
        {
            auto interface = getInterfaceFromVontext(ctx);
            return interface->beiGetNotFungibleAssetIDs(addressOffset, assetnameOffset, length, resultOffset, resultLength);
        }
#if HERA_DEBUGGING
        void print32(wasmer_instance_context_t *, uint32_t value)
        {
            HERA_DEBUG << "DEBUG print32: " << value << " " << hex << "0x" << value << dec << endl;
        }
        void print64(wasmer_instance_context_t *, uint64_t value)
        {
            HERA_DEBUG << "DEBUG print64: " << value << " " << hex << "0x" << value << dec << endl;
        }
        void printMem(wasmer_instance_context_t *ctx, uint32_t offset, uint32_t size)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->debugPrintMem(false, offset, size);
        }
        void printMemHex(wasmer_instance_context_t *ctx, uint32_t offset, uint32_t size)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->debugPrintMem(true, offset, size);
        }
        void printStorage(wasmer_instance_context_t *ctx, uint32_t offset)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->debugPrintStorage(false, offset);
        }
        void printStorageHex(wasmer_instance_context_t *ctx, uint32_t offset)
        {
            auto interface = getInterfaceFromVontext(ctx);
            interface->debugPrintStorage(true, offset);
        }
#endif
        shared_ptr<vector<wasmer_import_t>> initImportes()
        {
            wasmer_byte_array ethModule = getNameArray("ethereum");
            shared_ptr<vector<wasmer_import_t>> imports(new vector<wasmer_import_t>(), [](auto p) {
            // Destroy the instances we created for wasmer
#if 0
                wasmer_memory_destroy((wasmer_memory_t *)p->at(0).value.memory);
                //for (size_t i = 1; i < p->size(); ++i)
#endif
                for (size_t i = 0; i < p->size(); ++i)
                {
                    wasmer_import_func_destroy((wasmer_import_func_t *)p->at(i).value.func);
                }
                delete p;
            });

            imports->reserve(36);
#if 0
            // import memory
            wasmer_memory_t *memory = create_wasmer_memory();
            imports->emplace_back(wasmer_import_t{getNameArray("env"), getNameArray("memory"), wasmer_import_export_kind::WASM_MEMORY, {NULL}});
            imports->back().value.memory = memory;
#endif
            imports->emplace_back(
                wasmer_import_t{ethModule, getNameArray("useGas"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))beiUseGas, i64, 1, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{ethModule, getNameArray("getGasLeft"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiGetGasLeft, NULL, 0, i64, 1)}});
            imports->emplace_back(wasmer_import_t{ethModule, getNameArray("getAddress"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiGetAddress, i32, 1, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{ethModule, getNameArray("getExternalBalance"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiGetExternalBalance, i32_2, 2, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{ethModule, getNameArray("getBlockHash"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiGetBlockHash, i64_i32, 2, i32, 1)}});
            imports->emplace_back(wasmer_import_t{ethModule, getNameArray("getCallDataSize"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiGetCallDataSize, NULL, 0, i32, 1)}});
            imports->emplace_back(wasmer_import_t{ethModule, getNameArray("callDataCopy"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiCallDataCopy, i32_3, 3, i32, 1)}});
            imports->emplace_back(wasmer_import_t{ethModule, getNameArray("getCaller"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiGetCaller, i32, 1, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{ethModule, getNameArray("getCallValue"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiGetCallValue, i32, 1, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{ethModule, getNameArray("codeCopy"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiCodeCopy, i32_3, 3, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{ethModule, getNameArray("getCodeSize"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiGetCodeSize, NULL, 0, i32, 1)}});
            imports->emplace_back(wasmer_import_t{ethModule, getNameArray("externalCodeCopy"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiExternalCodeCopy, i32_4, 4, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{ethModule, getNameArray("getExternalCodeSize"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiGetExternalCodeSize, i32, 1, i32, 1)}});
            imports->emplace_back(wasmer_import_t{ethModule, getNameArray("getBlockCoinbase"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiGetBlockCoinbase, i32, 1, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{ethModule, getNameArray("getBlockDifficulty"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiGetBlockDifficulty, i32, 1, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{ethModule, getNameArray("getBlockGasLimit"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiGetBlockGasLimit, NULL, 0, i64, 1)}});
            imports->emplace_back(wasmer_import_t{ethModule, getNameArray("getTxGasPrice"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiGetTxGasPrice, i32, 1, NULL, 0)}});
            imports->emplace_back(
                wasmer_import_t{ethModule, getNameArray("log"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiLog, i32_7, 7, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{ethModule, getNameArray("getBlockNumber"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiGetBlockNumber, NULL, 0, i64, 1)}});
            imports->emplace_back(wasmer_import_t{ethModule, getNameArray("getBlockTimestamp"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiGetBlockTimestamp, NULL, 0, i64, 1)}});
            imports->emplace_back(wasmer_import_t{ethModule, getNameArray("getTxOrigin"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiGetTxOrigin, i32, 1, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{ethModule, getNameArray("storageStore"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiStorageStore, i32_2, 2, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{ethModule, getNameArray("storageLoad"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiStorageLoad, i32_2, 2, NULL, 0)}});
            imports->emplace_back(
                wasmer_import_t{ethModule, getNameArray("finish"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiFinish, i32_2, 2, NULL, 0)}});
            imports->emplace_back(
                wasmer_import_t{ethModule, getNameArray("revert"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiRevert, i32_2, 2, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{ethModule, getNameArray("getReturnDataSize"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiGetReturnDataSize, NULL, 0, i32, 1)}});
            imports->emplace_back(wasmer_import_t{ethModule, getNameArray("returnDataCopy"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiReturnDataCopy, i32_3, 3, NULL, 0)}});
            imports->emplace_back(
                wasmer_import_t{ethModule, getNameArray("create"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiCreate, i32_4, 4, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{ethModule, getNameArray("selfDestruct"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiSelfDestruct, i32, 1, NULL, 0)}});

            wasmer_byte_array bcosModule = getNameArray("bcos");
            imports->emplace_back(wasmer_import_t{bcosModule, getNameArray("useGas"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))beiUseGas, i64, 1, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{bcosModule, getNameArray("finish"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiFinish, i32_2, 2, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{bcosModule, getNameArray("getCallDataSize"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiGetCallDataSize, NULL, 0, i32, 1)}});
            imports->emplace_back(wasmer_import_t{bcosModule, getNameArray("getCallData"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))beiGetCallData, i32, 1, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{bcosModule, getNameArray("setStorage"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))beiSetStorage, i32_4, 4, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{bcosModule, getNameArray("getStorage"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))beiGetStorage, i32_3, 3, i32, 1)}});
            imports->emplace_back(wasmer_import_t{bcosModule, getNameArray("getCaller"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiGetCaller, i32, 1, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{bcosModule, getNameArray("revert"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiRevert, i32_2, 2, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{bcosModule, getNameArray("getTxOrigin"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiGetTxOrigin, i32, 1, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{bcosModule, getNameArray("getBlockNumber"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiGetBlockNumber, NULL, 0, i64, 1)}});
            imports->emplace_back(wasmer_import_t{bcosModule, getNameArray("getBlockTimestamp"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiGetBlockTimestamp, NULL, 0, i64, 1)}});
            imports->emplace_back(
                wasmer_import_t{bcosModule, getNameArray("log"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiLog, i32_7, 7, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{bcosModule, getNameArray("getReturnDataSize"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))eeiGetReturnDataSize, NULL, 0, i32, 1)}});
            imports->emplace_back(wasmer_import_t{bcosModule, getNameArray("getReturnData"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))beiReturnDataCopy, i32_3, 1, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{bcosModule, getNameArray("call"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))beiCall, i32_3, 3, i32, 1)}});
            // asset interfaces
            imports->emplace_back(wasmer_import_t{bcosModule, getNameArray("registerAsset"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))beiRegisterAsset, i32_4_i64_i32_2, 7, i32, 1)}});
            imports->emplace_back(wasmer_import_t{bcosModule, getNameArray("issueFungibleAsset"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))beiIssueFungibleAsset, i32_3_i64, 4, i32, 1)}});
            imports->emplace_back(wasmer_import_t{bcosModule, getNameArray("issueNotFungibleAsset"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))beiIssueNotFungibleAsset, i32_5, 5, i64, 1)}});
            imports->emplace_back(wasmer_import_t{bcosModule, getNameArray("transferAsset"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))beiTransferAsset, i32_3_i64_i32, 5, i32, 1)}});
            imports->emplace_back(wasmer_import_t{bcosModule, getNameArray("getAssetBanlance"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))beiGetAssetBanlance, i32_3, 3, i64, 1)}});
            imports->emplace_back(wasmer_import_t{bcosModule, getNameArray("getNotFungibleAssetIDs"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))beiGetNotFungibleAssetIDs, i32_5, 5, i32, 1)}});

#if HERA_DEBUGGING
            wasmer_byte_array debugModule = getNameArray("debug");
            imports->emplace_back(wasmer_import_t{debugModule, getNameArray("print32"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))print32, i32, 1, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{debugModule, getNameArray("print64"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))print64, i64, 1, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{debugModule, getNameArray("printMem"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))printMem, i32_2, 2, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{debugModule, getNameArray("printMemHex"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))printMemHex, i32_2, 2, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{debugModule, getNameArray("printStorage"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))printStorage, i32, 1, NULL, 0)}});
            imports->emplace_back(wasmer_import_t{debugModule, getNameArray("printStorageHex"), wasmer_import_export_kind::WASM_FUNCTION, {wasmer_import_func_new((void (*)(void *))printStorageHex, i32, 1, NULL, 0)}});
#endif
            return imports;
        }
    } // namespace
    static const set<string> eeiFunctions{"useGas", "getGasLeft", "getAddress", "getExternalBalance",
                                          "getBlockHash", "getCallDataSize", "callDataCopy", "getCaller", "getCallValue", "codeCopy",
                                          "getCodeSize", "externalCodeCopy", "getExternalCodeSize", "getBlockCoinbase",
                                          "getBlockDifficulty", "getBlockGasLimit", "getTxGasPrice", "log", "getBlockNumber",
                                          "getBlockTimestamp", "getTxOrigin", "storageStore", "storageLoad", "finish", "revert",
                                          "getReturnDataSize", "returnDataCopy", "call", "callCode", "callDelegate", "callStatic",
                                          "create", "selfDestruct"};
    static const set<string> beiFunctions{"finish", "getCallDataSize", "getCallData", "setStorage",
                                          "getStorage", "getCaller", "revert", "getTxOrigin", "log", "getReturnDataSize", "getReturnData"};
    void WasmerEngine::verifyContract(bytes_view code)
    {
        auto codeData = new unsigned char[code.size()];
        memcpy(codeData, code.data(), code.size());
        wasmer_module_t *module;
        auto compile_result = wasmer_compile(&module, codeData, (unsigned int)code.size());

        ensureCondition(compile_result == wasmer_result_t::WASMER_OK, ContractValidationFailure,
                        "Compile wasm failed.");
        wasmer_export_descriptors_t *exports;
        wasmer_export_descriptors(module, &exports);
        auto len = wasmer_export_descriptors_len(exports);
        int isBCIExported = 1;
        for (int i = 0; i < len; ++i)
        {
            auto exportObj = wasmer_export_descriptors_get(exports, i);
            auto nameBytes = wasmer_export_descriptor_name(exportObj);
            string objectName((char *)nameBytes.bytes, nameBytes.bytes_len);
            if (objectName == "memory")
            { // multiple memories are not supported for wasmer 0.17.0
                ensureCondition(
                    wasmer_export_descriptor_kind(exportObj) == wasmer_import_export_kind::WASM_MEMORY,
                    ContractValidationFailure, "\"memory\" is not pointing to memory.");
            }
            else if (objectName == "deploy" || objectName == "main" || objectName == "hash_type")
            {
                isBCIExported <<= 1;
                ensureCondition(wasmer_export_descriptor_kind(exportObj) ==
                                    wasmer_import_export_kind::WASM_FUNCTION,
                                ContractValidationFailure, "\"main\" is not pointing to function.");
            }
            else if (objectName == "__data_end" || objectName == "__heap_base")
            {
                ensureCondition(
                    wasmer_export_descriptor_kind(exportObj) == wasmer_import_export_kind::WASM_GLOBAL,
                    ContractValidationFailure, "__data_end/__heap_base is not pointing to global.");
            }
            else
            {
                HERA_DEBUG << "Invalid export is " << objectName << "\n";
                ensureCondition(false, ContractValidationFailure, "Invalid export is present.");
            }
        }
        if (isBCIExported != 1 << 3)
        {
            ensureCondition(false, ContractValidationFailure, "BCI(deploy/main/hash_type) are not all exported.");
        }
        wasmer_export_descriptors_destroy(exports);
        wasmer_import_descriptors_t *imports;
        wasmer_import_descriptors(module, &imports);
        auto importsLength = wasmer_import_descriptors_len(imports);

        for (unsigned int i = 0; i < importsLength; ++i)
        {
            auto importObj = wasmer_import_descriptors_get(imports, i);
            auto moduleNameBytes = wasmer_import_descriptor_module_name(importObj);
            string moduleName((char *)moduleNameBytes.bytes, moduleNameBytes.bytes_len);
#if HERA_DEBUGGING
            if (moduleName == "debug")
                continue;
#endif
            ensureCondition(moduleName == "bcos" || moduleName == "ethereum", ContractValidationFailure,
                            "Import from invalid namespace.");
            auto nameBytes = wasmer_import_descriptor_name(importObj);
            string objectName((char *)nameBytes.bytes, nameBytes.bytes_len);
            ensureCondition(beiFunctions.count(objectName) || eeiFunctions.count(objectName),
                            ContractValidationFailure, "Importing invalid EEI method.");
            ensureCondition(
                wasmer_import_descriptor_kind(importObj) == wasmer_import_export_kind::WASM_FUNCTION,
                ContractValidationFailure, "Imported function type mismatch.");
        }
        wasmer_import_descriptors_destroy(imports);
        wasmer_module_destroy(module);
    }

    ExecutionResult WasmerEngine::execute(evmc::HostContext &context, bytes_view code,
                                          bytes_view state_code, evmc_message const &msg, bool meterInterfaceGas)
    {
        instantiationStarted();
        HERA_DEBUG << "Executing with wasmer...\n";
        // Set up interface to eei host functions
        ExecutionResult result;
        WasmerEthereumInterface interface{context, state_code, msg, result, meterInterfaceGas};
        // Define an array containing our imports
        auto imports = initImportes();
        // Instantiate a WebAssembly Instance from Wasm bytes and imports
        wasmer_instance_t *instance = NULL;
        // TODO: check if need free codeData, for me it seems wasmer will free
        auto codeData = new unsigned char[code.size()];
        memcpy(codeData, code.data(), code.size());
        HERA_DEBUG << "Compile wasm code use wasmer...\n";
        wasmer_result_t compile_result =
            wasmer_instantiate(&instance,                                       // Our reference to our Wasm instance
                               codeData,                                        // The bytes of the WebAssembly modules
                               (uint32_t)code.size(),                           // The length of the bytes of the WebAssembly module
                               static_cast<wasmer_import_t *>(imports->data()), // The Imports array the will be used
                                                                                // as our importObject
                               (int32_t)imports->size()                         // The number of imports in the imports array
            );

        ensureCondition(compile_result == wasmer_result_t::WASMER_OK, ContractValidationFailure,
                        string("Compile wasm failed, ") + getWasmerErrorString());

        // Assert the Wasm instantion completed
        wasmer_instance_context_data_set(instance, (void *)&interface);
        auto ctx = wasmer_instance_context_get(instance);
        auto memory = wasmer_instance_context_memory(ctx, 0);
        ensureCondition(memory != NULL, InvalidMemoryAccess,
                        string("get memory from wasmer failed, ") + getWasmerErrorString());
        HERA_DEBUG << "wasmer memory pages is " << wasmer_memory_length(memory) << "\n";
        ensureCondition(wasmer_memory_length(memory) >= 1, InvalidMemoryAccess,
                        string("wasmer memory pages must greater than 1"));
        interface.setWasmContext(ctx);
        interface.setWasmMemory(memory);
        // Call the Wasm function
        wasmer_result_t call_result = wasmer_result_t::WASMER_ERROR;
        wasmer_value_t results[] = {};
        // Define our parameters (none) we are passing into the guest Wasm function call.
        wasmer_value_t params[] = {};
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
            wasmer_value_t hashTypeResult[1];
            call_result = wasmer_instance_call(instance, "hash_type", params, 0, hashTypeResult, 1);
            ensureCondition(call_result == wasmer_result_t::WASMER_OK, ContractValidationFailure,
                            "call hash_type failed, because of " + getWasmerErrorString());
            HERA_DEBUG << "Contract hash algorithm is " << (hashTypeResult[0].value.I32 ? "sm3\n" : "keccak256\n");
            // 0:keccak256, 1:sm3
            ensureCondition(hashTypeResult[0].value.I32 == useSM3Hash, ContractValidationFailure,
                            "hash type mismatch");
        }
        try
        {
            HERA_DEBUG << "Executing contract " << callName << "...\n";
            call_result = wasmer_instance_call(instance, // Our Wasm Instance
                                               callName, // the name of the exported function we want to call on the guest Wasm module
                                               params,   // Our array of parameters
                                               0,        // The number of parameters
                                               results,  // Our array of results
                                               0         // The number of results
            );
        }
        catch (EndExecution const &)
        {
            // This exception is ignored here because we consider it to be a success.
            // It is only a clutch for POSIX style exit()
        }
        // TODO: check if we need process wasmer_result_t
        // ensureCondition(call_result == wasmer_result_t::WASMER_OK, EndExecution, string("Call main
        // failed, ") + getWasmerErrorString());
        if (msg.kind == EVMC_CREATE)
        {
            result.returnValue = code;
        }

        auto errorMessage = getWasmerErrorString();
        wasmer_instance_destroy(instance);
        executionFinished();

        if (call_result != wasmer_result_t::WASMER_OK)
        {
            result.isRevert = true;
            HERA_DEBUG << "call " << callName << " failed, error message:" << errorMessage << "\n";
            //TODO: throw specific exception according to error message
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
            else if (errorMessage.find(REVERT) != std::string::npos)
            {
                HERA_DEBUG << REVERT << "\n";
            }
            else
            {
                throw std::runtime_error("Unknown error.");
            }
        }
        else
        { // FIXME: debug log should delete before release
            HERA_DEBUG << "Output size is " << result.returnValue.size() << ", ouput=";
            for (size_t i = 0; i < result.returnValue.size(); ++i)
            {
                HERA_DEBUG << hex << result.returnValue[i];
            }
            HERA_DEBUG << " done\n";
        }

        return result;
    };
} // namespace hera
