#pragma once
#include "models.h"
#include "logger.h"

#include <vector>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <nlohmann/json.hpp>

class Storage {
public:
    Storage(const std::string& dbPath,
            const std::string& backupPath,
            Logger& logger)
        : dbPath_(dbPath), backupPath_(backupPath), logger_(logger) {
        std::filesystem::create_directories(std::filesystem::path(dbPath_).parent_path());
    }

    std::vector<Sport> loadAll() {
        std::vector<Sport> result;

        if (!std::filesystem::exists(dbPath_)) {
            createEmptyJsonFile(dbPath_);
            logger_.warning("DB file not found. Created new empty JSON file.");
            return result;
        }

        std::ifstream in(dbPath_);
        if (!in) {
            logger_.error("Failed to open DB file for reading.");
            throw std::runtime_error("Ошибка чтения файла БД");
        }

        nlohmann::json root;
        try {
            in >> root;
        } catch (const std::exception& e) {
            logger_.error(std::string("JSON parse error: ") + e.what());
            throw std::runtime_error("Файл БД поврежден или имеет неверный JSON-формат");
        }

        if (!root.is_array()) {
            logger_.error("DB JSON root is not an array.");
            throw std::runtime_error("Некорректная структура JSON-файла БД");
        }

        for (const auto& item : root) {
            Sport s;
            s.sport_id = item.value("sport_id", 0);
            s.name = item.value("name", "");
            s.category = item.value("category", "");
            s.olympic_status = item.value("olympic_status", false);
            s.description = item.value("description", "");
            s.governing_body = item.value("governing_body", "");
            s.image_path = item.value("image_path", "");
            s.medical_contraindications = item.value("medical_contraindications", "");
            s.weight = item.value("weight", 1); // если поля нет — будет 1

            result.push_back(s);
        }

        logger_.info("Loaded records from JSON: " + std::to_string(result.size()));
        return result;
    }

    void saveAll(const std::vector<Sport>& data) {
        createBackup();

        nlohmann::json root = nlohmann::json::array();

        for (const auto& s : data) {
            root.push_back({
                {"sport_id", s.sport_id},
                {"name", s.name},
                {"category", s.category},
                {"olympic_status", s.olympic_status},
                {"description", s.description},
                {"governing_body", s.governing_body},
                {"image_path", s.image_path},
                {"medical_contraindications", s.medical_contraindications},
                {"weight", s.weight}
            });
        }

        std::ofstream out(dbPath_, std::ios::trunc);
        if (!out) {
            logger_.error("Failed to open DB file for writing.");
            throw std::runtime_error("Ошибка записи файла БД");
        }

        out << root.dump(2);
        logger_.info("Saved records to JSON: " + std::to_string(data.size()));
    }

private:
    std::string dbPath_;
    std::string backupPath_;
    Logger& logger_;

    void createBackup() {
        if (std::filesystem::exists(dbPath_)) {
            std::filesystem::copy_file(
                dbPath_,
                backupPath_,
                std::filesystem::copy_options::overwrite_existing
            );
            logger_.info("Backup created: " + backupPath_);
        }
    }

    static void createEmptyJsonFile(const std::string& path) {
        std::ofstream out(path, std::ios::trunc);
        if (!out) {
            throw std::runtime_error("Не удалось создать новый JSON-файл БД");
        }
        out << "[]";
    }
};
