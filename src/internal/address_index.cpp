//
//  address_index.cpp
//  blocksci_address_index
//
//  Created by Harry Kalodner on 7/9/17.
//
//

#define BLOCKSCI_WITHOUT_SINGLETON

#include "address_index.hpp"
#include "address_info.hpp"
#include "column_iterator.hpp"
#include "dedup_address.hpp"
#include "memory_view.hpp"

#include <blocksci/core/inout_pointer.hpp>
#include <blocksci/core/raw_address.hpp>

#include <range/v3/view/transform.hpp>
#include <range/v3/range_for.hpp>

#include <sstream>

namespace blocksci {
    
    AddressIndex::AddressIndex(const filesystem::path &path, bool readonly) {
        rocksdb::Options options;
        // Optimize RocksDB. This is the easiest way to get RocksDB to perform well
        options.IncreaseParallelism();
        options.OptimizeLevelStyleCompaction();
        // create the DB if it's not already present
        options.create_if_missing = true;
        options.create_missing_column_families = true;
        
        
        std::vector<rocksdb::ColumnFamilyDescriptor> columnDescriptors;
        blocksci::for_each(AddressType::all(), [&](auto tag) {
            std::stringstream ss;
            ss << addressName(tag) << "_output";
            columnDescriptors.emplace_back(ss.str(), rocksdb::ColumnFamilyOptions{});
        });
        blocksci::for_each(AddressType::all(), [&](auto tag) {
            std::stringstream ss;
            ss << addressName(tag) << "_nested";
            columnDescriptors.emplace_back(ss.str(), rocksdb::ColumnFamilyOptions{});
        });
        columnDescriptors.emplace_back(rocksdb::kDefaultColumnFamilyName, rocksdb::ColumnFamilyOptions());
        
        rocksdb::DB *dbPtr;
        std::vector<rocksdb::ColumnFamilyHandle *> columnHandlePtrs;
        if (readonly) {
            rocksdb::Status s = rocksdb::DB::OpenForReadOnly(options, path.str().c_str(), columnDescriptors, &columnHandlePtrs, &dbPtr);
            if (!s.ok()) {
                throw std::runtime_error{"Could not open address index with error: " + std::string{s.getState()}};
            }
        } else {
            rocksdb::Status s = rocksdb::DB::Open(options, path.str().c_str(), columnDescriptors, &columnHandlePtrs, &dbPtr);
            if (!s.ok()) {
                throw std::runtime_error{"Could not open address index with error: " + std::string{s.getState()}};
            }
        }
        db = std::unique_ptr<rocksdb::DB>(dbPtr);
        for (auto handle : columnHandlePtrs) {
            columnHandles.emplace_back(std::unique_ptr<rocksdb::ColumnFamilyHandle>(handle));
        }
    }
    
    AddressIndex::~AddressIndex() = default;
    
    void AddressIndex::compactDB() {
        for (auto &column : columnHandles) {
            db->CompactRange(rocksdb::CompactRangeOptions{}, column.get(), nullptr, nullptr);
        }
    }
    
    const std::unique_ptr<rocksdb::ColumnFamilyHandle> &AddressIndex::getOutputColumn(AddressType::Enum type) const {
        return columnHandles[static_cast<size_t>(type)];
    }
    
    const std::unique_ptr<rocksdb::ColumnFamilyHandle> &AddressIndex::getNestedColumn(AddressType::Enum type) const {
        return columnHandles[AddressType::size + static_cast<size_t>(type)];
    }
    
    ranges::any_view<InoutPointer> AddressIndex::getOutputPointers(const RawAddress &address) const {
        auto prefixData = reinterpret_cast<const char *>(&address.scriptNum);
        std::vector<char> prefix(prefixData, prefixData + sizeof(address.scriptNum));
        auto rawOutputPointerRange = ColumnIterator(db.get(), getOutputColumn(address.type).get(), prefix);
        return rawOutputPointerRange | ranges::view::transform([](std::pair<MemoryView, MemoryView> pair) -> InoutPointer {
            auto &key = pair.first;
            key.data += sizeof(uint32_t);
            InoutPointer outPoint;
            memcpy(&outPoint, key.data, sizeof(outPoint));
            return outPoint;
        });
    }
    
    ranges::any_view<RawAddress> AddressIndex::getIncludingMultisigs(const RawAddress &searchAddress) const {
        if (dedupType(searchAddress.type) != DedupAddressType::PUBKEY) {
            return {};
        }
        
        auto prefixData = reinterpret_cast<const char *>(&searchAddress.scriptNum);
        std::vector<char> prefix(prefixData, prefixData + sizeof(searchAddress.scriptNum));
        auto rawDedupAddressRange = ColumnIterator(db.get(), getNestedColumn(AddressType::MULTISIG_PUBKEY).get(), prefix);
        return rawDedupAddressRange | ranges::view::transform([](std::pair<MemoryView, MemoryView> pair) -> RawAddress {
            auto &key = pair.first;
            key.data += sizeof(uint32_t);
            DedupAddress rawParent;
            memcpy(&rawParent, key.data, sizeof(rawParent));
            return RawAddress{rawParent.scriptNum, AddressType::MULTISIG};
        });
    }
    
    void AddressIndex::addNestedAddresses(std::vector<std::pair<blocksci::RawAddress, blocksci::DedupAddress>> nestedCache) {
        rocksdb::WriteBatch batch;
        for (auto &pair : nestedCache) {
            const RawAddress &childAddress = pair.first;
            const DedupAddress &parentAddress = pair.second;
            std::array<rocksdb::Slice, 2> keyParts = {{
                rocksdb::Slice(reinterpret_cast<const char *>(&childAddress.scriptNum), sizeof(childAddress.scriptNum)),
                rocksdb::Slice(reinterpret_cast<const char *>(&parentAddress), sizeof(parentAddress))
            }};
            std::string sliceStr;
            rocksdb::Slice key{rocksdb::SliceParts{keyParts.data(), keyParts.size()}, &sliceStr};
            auto &nestedColumn = getNestedColumn(childAddress.type);
            batch.Put(nestedColumn.get(), key, rocksdb::Slice{});
        }
        writeBatch(batch);
    }
    
    void AddressIndex::addOutputAddresses(std::vector<std::pair<RawAddress, InoutPointer>> outputCache) {
        rocksdb::WriteBatch batch;
        for (auto &pair : outputCache) {
            const RawAddress &address = pair.first;
            const InoutPointer &pointer = pair.second;
            std::array<rocksdb::Slice, 2> keyParts = {{
                rocksdb::Slice(reinterpret_cast<const char *>(&address.scriptNum), sizeof(address.scriptNum)),
                rocksdb::Slice(reinterpret_cast<const char *>(&pointer), sizeof(pointer))
            }};
            std::string sliceStr;
            rocksdb::Slice key{rocksdb::SliceParts{keyParts.data(), keyParts.size()}, &sliceStr};
            auto &outputColumn = getOutputColumn(address.type);
            batch.Put(outputColumn.get(), key, rocksdb::Slice{});
        }
        writeBatch(batch);
    }
    
    void AddressIndex::rollback(uint32_t txNum) {
        for_each(AddressType::all(), [&](auto type) {
            auto &column = getOutputColumn(type);
            auto it = getOutputIterator(type);
            rocksdb::WriteBatch batch;
            for (it->SeekToFirst(); it->Valid(); it->Next()) {
                auto key = it->key();
                key.remove_prefix(sizeof(uint32_t));
                InoutPointer outPoint;
                memcpy(&outPoint, key.data(), sizeof(outPoint));
                if (outPoint.txNum >= txNum) {
                    batch.Delete(column.get(), it->key());
                }
            }
            assert(it->status().ok()); // Check for any errors found during the scan
        });
    }
    
    bool AddressIndex::checkIfExists(const RawAddress &address) const {
        rocksdb::Slice key{reinterpret_cast<const char *>(&address.scriptNum), sizeof(address.scriptNum)};
        {
            auto it = getOutputIterator(address.type);
            it->Seek(key);
            if (it->Valid() && it->key().starts_with(key)) {
                return true;
            }
        }
        {
            auto it = getNestedIterator(address.type);
            it->Seek(key);
            if (it->Valid() && it->key().starts_with(key)) {
                return true;
            }
        }
        return false;
    }
    
    bool AddressIndex::checkIfTopLevel(const RawAddress &address) const {
        rocksdb::Slice key{reinterpret_cast<const char *>(&address.scriptNum), sizeof(address.scriptNum)};
        auto it = getOutputIterator(address.type);
        it->Seek(key);
        if (it->Valid() && it->key().starts_with(key)) {
            return true;
        }
        return false;
    }
    
    std::unordered_set<RawAddress> AddressIndex::getPossibleNestedEquivalentUp(const RawAddress &searchAddress) const {
        std::unordered_set<RawAddress> addressesToSearch{searchAddress};
        std::unordered_set<RawAddress> searchedAddresses;
        while (addressesToSearch.size() > 0) {
            auto setIt = addressesToSearch.begin();
            auto address = *setIt;
            rocksdb::Slice key{reinterpret_cast<const char *>(&address.scriptNum), sizeof(address.scriptNum)};
            auto it = getNestedIterator(address.type);
            for (it->Seek(key); it->Valid() && it->key().starts_with(key); it->Next()) {
                auto foundKey = it->key();
                foundKey.remove_prefix(sizeof(uint32_t));
                DedupAddress rawParent;
                memcpy(&rawParent, foundKey.data(), sizeof(rawParent));
                if (rawParent.type == DedupAddressType::SCRIPTHASH) {
                    for (auto type : equivAddressTypes(equivType(AddressType::SCRIPTHASH))) {
                        RawAddress newAddress{rawParent.scriptNum, type};
                        if (searchedAddresses.find(newAddress) == searchedAddresses.end()) {
                            addressesToSearch.insert(newAddress);
                        }
                    }
                }
            }
            searchedAddresses.insert(address);
            addressesToSearch.erase(setIt);
        }
        
        return searchedAddresses;
    }
}
