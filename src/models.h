#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct Sport {
    int32_t sport_id = 0;
    std::string name;
    std::string category;
    bool olympic_status = false;
    std::string description;
    std::string governing_body;
    std::string image_path;
    std::string medical_contraindications;

    // В файл v1.0 НЕ пишем, живет только в памяти
    int weight = 1;
};

enum class SortField {
    None,
    Id,
    Name,
    Category,
    OlympicStatus,
    Description,
    GoverningBody,
    ImagePath,
    MedicalContraindications
};

inline std::string sortFieldToString(SortField f) {
    switch (f) {
        case SortField::Id: return "sport_id";
        case SortField::Name: return "name";
        case SortField::Category: return "category";
        case SortField::OlympicStatus: return "olympic_status";
        case SortField::Description: return "description";
        case SortField::GoverningBody: return "governing_body";
        case SortField::ImagePath: return "image_path";
        case SortField::MedicalContraindications: return "medical_contraindications";
        default: return "none";
    }
}

inline SortField sortFieldFromString(const std::string& s) {
    if (s == "sport_id") return SortField::Id;
    if (s == "name") return SortField::Name;
    if (s == "category") return SortField::Category;
    if (s == "olympic_status") return SortField::OlympicStatus;
    if (s == "description") return SortField::Description;
    if (s == "governing_body") return SortField::GoverningBody;
    if (s == "image_path") return SortField::ImagePath;
    if (s == "medical_contraindications") return SortField::MedicalContraindications;
    return SortField::None;
}
