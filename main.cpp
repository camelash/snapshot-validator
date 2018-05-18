#include <iostream>
#include <iomanip>
#include <sstream>
#include <pqxx/pqxx>
#include <sodium.h>
#include <chrono>
#include <unordered_map>

#include "blockheader.h"
#include "payload.h"
#include "transaction.h"

inline std::vector<unsigned char> hex2Bytes(std::string hex) {
    std::vector<unsigned char> out(hex.size() / 2);
    for (int i = 0; i < hex.size(); i += 2)
    {
        std::istringstream iss(hex.substr(i, 2));
        int temp;
        iss >> std::hex >> temp;
        out[i/2] = static_cast<unsigned char>(temp);
    }
    return out;
}

std::string bytes2Hex(std::vector<unsigned char> data) {
    std::stringstream out;
    out << std::setfill('0') << std::hex;
    for(size_t i = 0; i < data.size(); ++i) {
        out << std::setw(2) << static_cast<int>(data[i]);
    }
    return out.str();
}

inline std::vector<unsigned char> asVector(const pqxx::binarystring &binstr) {
    return std::vector<unsigned char>(
                reinterpret_cast<const unsigned char*>(binstr.get()),
                reinterpret_cast<const unsigned char*>(binstr.get() + binstr.length())
    );
}

class ScopedBenchmark {
public:
    ScopedBenchmark(std::string title)
        : start_(std::chrono::steady_clock::now())
        , title_(title)
    {}

    ~ScopedBenchmark() {
        auto diff = std::chrono::steady_clock::now() - start_;
        std::cout << title_ << " finished in " << std::chrono::duration<double, std::milli>(diff).count() << " ms" << std::endl;
    }

private:
    std::chrono::steady_clock::time_point start_;
    std::string title_;
};

int main()
{
    if (sodium_init() == -1) {
        return 1;
    }

    try
    {
        pqxx::connection C("dbname=lisk_beta");
        std::cout << "Connected to database " << C.dbname() << std::endl;
        pqxx::read_transaction transaction(C);

        {
            pqxx::result R = transaction.exec("SELECT COUNT(*) as number FROM trs");
            for (auto row : R) std::cout << "Transaction count " << row[0].c_str() << std::endl;
        }

        {
            pqxx::result R = transaction.exec("SELECT COUNT(*) as number FROM blocks");
            for (auto row : R) std::cout << "Blocks count " << row[0].c_str() << std::endl;
        }

        {
            pqxx::result R = transaction.exec("SELECT MAX(height) FROM blocks");
            for (auto row : R) std::cout << "Height: " << row[0].c_str() << std::endl;
        }


        std::unordered_map<std::uint64_t, std::vector<std::pair<Transaction, std::vector<unsigned char>>>> blockToTransactions;

        {
            std::cout << "Reading transactions ..." << std::endl;
            ScopedBenchmark benchmark("Reading transactions");

            pqxx::result R = transaction.exec(R"SQL(
                SELECT
                    id, "blockId", type, timestamp, "senderPublicKey", "recipientId", amount, signature,
                    transfer.data AS type0Asset,
                    multisignatures.keysgroup AS type1Asset,
                    delegates.username AS type2Asset
                FROM trs
                LEFT JOIN transfer ON trs.id = transfer."transactionId"
                LEFT JOIN multisignatures ON trs.id = multisignatures."transactionId"
                LEFT JOIN delegates ON trs.id = delegates."transactionId"
                ORDER BY "rowId"
            )SQL");
            for (auto row : R) {
                // Read fields in row
                int index = 0;
                auto dbId = row[index++].as<std::uint64_t>();
                auto dbBockId = row[index++].as<std::uint64_t>();
                auto dbType = row[index++].as<int>();
                auto dbTimestamp = row[index++].as<std::uint32_t>();
                auto dbSenderPublicKey = pqxx::binarystring(row[index++]);
                auto dbRecipientIdString = row[index++].get<std::string>();
                auto dbAmount = row[index++].as<std::uint64_t>();
                auto dbSignature = pqxx::binarystring(row[index++]);
                auto dbType0Asset = pqxx::binarystring(row[index++]);
                auto dbType1Asset = row[index++].get<std::string>();
                auto dbType2Asset = row[index++].get<std::string>();

                // Parse fields in row
                auto senderPublicKey = asVector(dbSenderPublicKey);
                auto signature = asVector(dbSignature);

                const unsigned char* assetDataBegin = nullptr;
                std::size_t assetDataLength = 0;
                switch (dbType) {
                case 0:
                    assetDataBegin = reinterpret_cast<const unsigned char*>(dbType0Asset.get());
                    assetDataLength = dbType0Asset.length();
                    break;
                case 1:
                    if (dbType1Asset) {
                        assetDataBegin = reinterpret_cast<const unsigned char*>(dbType1Asset->data());
                        assetDataLength = dbType1Asset->size();
                    }
                    break;
                case 2:
                    if (dbType2Asset) {
                        assetDataBegin = reinterpret_cast<const unsigned char*>(dbType2Asset->data());
                        assetDataLength = dbType2Asset->size();
                    }
                    break;
                }
                std::uint64_t recipientId;
                if (dbRecipientIdString) {
                    std::istringstream iss(dbRecipientIdString->substr(0, dbRecipientIdString->size()-1));
                    iss >> recipientId;
                }
                else {
                    recipientId = 0;
                }

                //if (type == 2 && timestamp == 0) {
                //    amount = 25;
                //}

                // Use fields in row
                auto t = Transaction(
                    dbType,
                    dbTimestamp,
                    senderPublicKey,
                    recipientId,
                    dbAmount,
                    assetDataBegin,
                    assetDataLength
                );
                blockToTransactions[dbBockId].push_back({t, signature});
                //blockToTransactions.insert({dbId, t});

                if (dbType == 0 || (dbType == 2 && dbTimestamp != 0)) {
                    auto calculatedId = t.id(signature);

                    //if (dbId != calculatedId) {
                    //    throw std::runtime_error("Transaction ID mismatch");
                    //}

                    auto hash = t.hash();

                    if (crypto_sign_verify_detached(signature.data(), hash.data(), hash.size(), senderPublicKey.data()) != 0) {
                        std::cout << "ID: " << dbId << std::endl;
                        std::cout << "Transaction: " << t << std::endl;
                        std::cout << "Pubkey: " << bytes2Hex(senderPublicKey) << std::endl;
                        std::cout << "Signature: " << bytes2Hex(signature) << std::endl;
                        throw std::runtime_error("Invalid signature");
                    } else {
                        // valid!
                        //std::cout << "Transaction: " << t << std::endl;
                        //std::cout << "Pubkey: " << bytes2Hex(std::vector<unsigned char>(senderPublicKeyBegin, senderPublicKeyBegin+senderPublicKeyLength)) << std::endl;
                        //std::cout << "Valid ID: " << dbId << "/" << calculatedId << std::endl;
                    }
                } else {
                    std::cout << "Transaction not verified: " << t << " ID: " << dbId << std::endl;
                }
            }
        }


        {
            std::cout << "Reading blocks ..." << std::endl;
            ScopedBenchmark benchmark("Reading blocks");

            pqxx::result R = transaction.exec(R"SQL(
                SELECT
                    id, version, timestamp, height, "previousBlock", "numberOfTransactions", "totalAmount", "totalFee", reward,
                    "payloadLength", "payloadHash", "generatorPublicKey", "blockSignature"
                FROM blocks
                ORDER BY height
            )SQL");

            std::uint64_t lastHeight = 0;
            std::uint64_t lastBlockId = 0;
            for (auto row : R) {
                int index = 0;
                const auto dbId = row[index++].as<std::uint64_t>();
                const auto dbVersion = row[index++].as<std::uint32_t>();
                const auto dbTimestamp = row[index++].as<std::uint32_t>();
                const auto dbHeight = row[index++].as<std::uint64_t>();
                const auto dbPreviousBlock = row[index++].get<std::uint64_t>();
                const auto dbNumberOfTransactions = row[index++].as<std::uint32_t>();
                const auto dbTotalAmount = row[index++].as<std::uint64_t>();
                const auto dbTotalFee = row[index++].as<std::uint64_t>();
                const auto dbReward = row[index++].as<std::uint64_t>();
                const auto dbPayloadLength = row[index++].as<std::uint32_t>();
                const auto dbPayloadHash = pqxx::binarystring(row[index++]);
                const auto dbGeneratorPublicKey = pqxx::binarystring(row[index++]);
                const auto dbSignature = pqxx::binarystring(row[index++]);

                const auto generatorPublicKey = asVector(dbGeneratorPublicKey);
                const auto payloadHash = asVector(dbPayloadHash);
                const auto signature = asVector(dbSignature);

                if (dbHeight != lastHeight + 1) {
                    throw std::runtime_error("Height mismatch");
                }
                lastHeight = dbHeight;

                if (dbHeight != 1) {
                    if (*dbPreviousBlock != lastBlockId) {
                        throw std::runtime_error("previous block mismatch");
                    }
                }
                lastBlockId = dbId;


                BlockHeader bh(
                    dbVersion,
                    dbTimestamp,
                    dbPreviousBlock ? *dbPreviousBlock : 0,
                    dbNumberOfTransactions,
                    dbTotalAmount,
                    dbTotalFee,
                    dbReward,
                    dbPayloadLength,
                    payloadHash,
                    generatorPublicKey
                );

                auto hash = bh.hash();
                if (crypto_sign_verify_detached(signature.data(), hash.data(), hash.size(), generatorPublicKey.data()) != 0) {
                    std::cout << "ID: " << dbId << std::endl;
                    std::cout << "Pubkey: " << bytes2Hex(generatorPublicKey) << std::endl;
                    std::cout << "Signature: " << bytes2Hex(signature) << std::endl;
                    throw std::runtime_error("Invalid signature");
                }

                Payload payload(blockToTransactions[dbId]);
                if (payload.transactionCount() != dbNumberOfTransactions) {
                    throw std::runtime_error("transaction number mismatch");
                }

                auto calculatedPayloadHash = payload.hash();
                if (payloadHash != calculatedPayloadHash) {
                    //throw std::runtime_error("payload hash mismatch");
                    std::cout << "payload hash mismatch" << std::endl;
                }

                auto id = bh.id(signature);
                if (id != dbId) {
                    throw std::runtime_error("id mismatch");
                }

                //std::cout << "Block: " << id << std::endl;
            }
        }


        transaction.commit();
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    return 0;
}
