#include "models.h"
#include "logger.h"
#include "storage.h"
#include "server.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "imgui.h"
#include "implot.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include <filesystem>
#include <iostream>
#include <vector>
#include <string>
#include <array>
#include <unordered_map>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cstdlib>
#include <algorithm>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(__linux__)
#  include <unistd.h>
#endif

// ========== ПРОТОТИПЫ ФУНКЦИЙ ==========
// Forward declaration to allow function prototypes to use UiState before its definition.
struct UiState;
static void deleteRecord(UiState& ui, int id);
static std::string sortFieldToBackendString(SortField field);
static bool fetchCategories(UiState& ui);
static void collectTreeInOrder(const nlohmann::json& node, std::vector<nlohmann::json>& out);

// ========== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ==========

// Получение пути к исполняемому файлу
static std::filesystem::path getExecutablePath() {
#if defined(_WIN32)
    // Windows: reliable way to get executable directory.
    std::wstring buf(MAX_PATH, L'\0');
    DWORD len = 0;
    for (;;) {
        len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) break;
        if (len < buf.size() - 1) break;
        buf.resize(buf.size() * 2);
    }
    if (len > 0) {
        buf.resize(len);
        return std::filesystem::path(buf).parent_path();
    }
    return std::filesystem::current_path();
#elif defined(__linux__)
    // Linux: /proc/self/exe points to the executable.
    std::string buf(4096, '\0');
    const ssize_t len = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (len > 0) {
        buf.resize(static_cast<size_t>(len));
        return std::filesystem::path(buf).parent_path();
    }
    return std::filesystem::current_path();
#else
    return std::filesystem::current_path();
#endif
}

// Настройка рабочей директории
struct ResolvedPaths {
    std::filesystem::path appRoot;   // where assets/ live; set as CWD
    std::filesystem::path dataRoot;  // where data/ live; storage uses absolute paths
};

static ResolvedPaths setupWorkingDirectory() {
    const std::filesystem::path exeDir = getExecutablePath();

    auto looksLikeAppRoot = [](const std::filesystem::path& p) -> bool {
        return std::filesystem::exists(p / "assets");
    };

    auto looksLikeDataRoot = [](const std::filesystem::path& p) -> bool {
        return std::filesystem::exists(p / "data" / "sports_database.dat") || std::filesystem::exists(p / "data");
    };

    auto findAppRootUpwards = [&](std::filesystem::path start) -> std::filesystem::path {
        std::filesystem::path cur = start;
        for (int i = 0; i < 8; ++i) {
            if (looksLikeAppRoot(cur)) return cur;
            if (looksLikeAppRoot(cur / "sports_app")) return cur / "sports_app"; // launched from repo root
            if (!cur.has_parent_path()) break;
            const auto parent = cur.parent_path();
            if (parent == cur) break;
            cur = parent;
        }
        return {};
    };

    auto findDataRootUpwards = [&](std::filesystem::path start) -> std::filesystem::path {
        std::filesystem::path cur = start;
        for (int i = 0; i < 8; ++i) {
            // Prefer "this folder has data/"
            if (looksLikeDataRoot(cur)) return cur;
            // Also check common nested app dir
            if (looksLikeDataRoot(cur / "sports_app")) return cur / "sports_app";
            if (!cur.has_parent_path()) break;
            const auto parent = cur.parent_path();
            if (parent == cur) break;
            cur = parent;
        }
        return {};
    };

    ResolvedPaths out;
    out.appRoot = findAppRootUpwards(exeDir);
    if (out.appRoot.empty()) out.appRoot = findAppRootUpwards(std::filesystem::current_path());

    // Data root can be different from app root (e.g., user keeps shared data/ at repo root).
    out.dataRoot = findDataRootUpwards(exeDir);
    if (out.dataRoot.empty()) out.dataRoot = findDataRootUpwards(std::filesystem::current_path());
    if (out.dataRoot.empty()) out.dataRoot = out.appRoot;

    if (!out.appRoot.empty()) {
        std::filesystem::current_path(out.appRoot);
    }

    std::cout << "Executable dir: " << exeDir << std::endl;
    std::cout << "Working directory: " << std::filesystem::current_path() << std::endl;
    std::cout << "Data root: " << out.dataRoot << std::endl;

    return out;
}

// ========== СТРУКТУРЫ ДАННЫХ ==========

struct TextureData {
    GLuint id = 0;
    int width = 0;
    int height = 0;
};

struct TreeTabState {
    nlohmann::json tree;
    char searchValue[256] = "";
    std::string status;
    nlohmann::json foundRecord;
    bool hasFoundRecord = false;
};

struct UiState {
    // Пагинация
    int page = 1;
    int limit = 10;
    int total = 0;
    std::vector<nlohmann::json> records;
    
    // Поиск и сортировка
    char searchName[256] = "";
    char sortField[64] = "name";
    bool sortAscending = true;
    
    // Форма добавления
    char sportId[32] = "";
    char name[256] = "";
    char category[256] = "";
    bool olympic = false;
    char description[512] = "";
    char governingBody[256] = "";
    char imagePath[512] = "";
    char contraindications[512] = "";
    std::string selectedImageSourcePath;
    std::string imagePickMessage;
    bool imagePickerOpen = false;
    char imagePickerDir[512] = "C:/Users/Egor/Desktop/images";
    
    // Статусы
    std::string status;
    std::string errorMessage;
    
    // Результаты поиска
    nlohmann::json binarySearchRecord;
    bool hasBinarySearchRecord = false;
    
    // Удаление (confirmation popup)
    bool deleteConfirmOpen = false;
    int pendingDeleteSportId = 0;
    
    // Деревья
    TreeTabState treeTabs[8];
    
    // UI настройки
    bool showAddForm = true;
    bool showRecords = true;
    bool showTree = true;
    bool showSearch = true;
    bool showStats = true;
    
    // Фильтры
    char filterCategory[128] = "";
    bool filterOlympic = false;
    bool filterNonOlympic = false;
    std::vector<std::string> knownCategories;
    
};

// ========== ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ==========

static std::unordered_map<std::string, TextureData> g_textureCache;
static ImVec4 g_successColor = ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
static ImVec4 g_errorColor = ImVec4(0.9f, 0.2f, 0.2f, 1.0f);
static ImVec4 g_warningColor = ImVec4(0.9f, 0.6f, 0.1f, 1.0f);
static ImVec4 g_primaryColor = ImVec4(0.2f, 0.5f, 0.9f, 1.0f);
static std::filesystem::path g_lastImageDirectory;
static const std::filesystem::path g_externalImagesDir =
    std::filesystem::path("C:/Users/Egor/Desktop/images");

static const std::array<SortField, 8> g_treeFields = {
    SortField::Id, SortField::Name, SortField::Category,
    SortField::OlympicStatus, SortField::Description,
    SortField::GoverningBody, SortField::ImagePath,
    SortField::MedicalContraindications
};

static const std::array<const char*, 8> g_treeLabels = {
    "ID", "Название", "Категория", "Олимпийский",
    "Описание", "Орган", "Изображение",
    "Противопоказания"
};

// ========== ФУНКЦИЯ ДЛЯ ПРЕОБРАЗОВАНИЯ SortField В СТРОКУ ==========
static std::string sortFieldToBackendString(SortField field) {
    switch (field) {
        case SortField::Id: return "sport_id";
        case SortField::Name: return "name";
        case SortField::Category: return "category";
        case SortField::OlympicStatus: return "olympic_status";
        case SortField::Description: return "description";
        case SortField::GoverningBody: return "governing_body";
        case SortField::ImagePath: return "image_path";
        case SortField::MedicalContraindications: return "medical_contraindications";
        default: return "name";
    }
}

static const char* sortFieldToHuman(SortField field) {
    switch (field) {
        case SortField::Id: return "ID (sport_id)";
        case SortField::Name: return "Название (name)";
        case SortField::Category: return "Категория (category)";
        case SortField::OlympicStatus: return "Олимпийский (olympic_status)";
        case SortField::Description: return "Описание (description)";
        case SortField::GoverningBody: return "Орган (governing_body)";
        case SortField::ImagePath: return "Изображение (image_path)";
        case SortField::MedicalContraindications: return "Противопоказания (medical_contraindications)";
        default: return "Не выбрано";
    }
}

static void setSortFieldInUi(UiState& ui, SortField field) {
    // Ключи для backend совпадают со строковым форматом из models.h
    const std::string key = sortFieldToString(field);
    std::strncpy(ui.sortField, key.c_str(), sizeof(ui.sortField));
    ui.sortField[sizeof(ui.sortField) - 1] = '\0';
}

// ========== ФУНКЦИИ РАБОТЫ С ТЕКСТУРАМИ ==========

static bool loadTextureFromFile(const std::string& path, TextureData& outTexture) {
    int width = 0, height = 0, channels = 0;
    unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!data) return false;

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    stbi_image_free(data);

    outTexture.id = texture;
    outTexture.width = width;
    outTexture.height = height;
    return true;
}

static TextureData* getTexture(const std::string& path) {
    if (path.empty() || !std::filesystem::exists(path)) return nullptr;
    
    auto it = g_textureCache.find(path);
    if (it != g_textureCache.end()) return &it->second;
    
    TextureData tex;
    if (!loadTextureFromFile(path, tex)) return nullptr;
    
    g_textureCache[path] = tex;
    return &g_textureCache[path];
}

static void destroyAllTextures() {
    for (auto& item : g_textureCache) {
        if (item.second.id != 0) glDeleteTextures(1, &item.second.id);
    }
    g_textureCache.clear();
}

// ========== ФУНКЦИИ UI ==========

static void drawImagePreview(const std::string& path, float maxWidth = 220.0f, float maxHeight = 160.0f) {
    if (path.empty()) {
        ImGui::TextUnformatted("📷 Нет изображения");
        return;
    }
    
    if (!std::filesystem::exists(path)) {
        ImGui::TextColored(g_warningColor, "⚠ Файл не найден: %s", path.c_str());
        return;
    }
    
    TextureData* tex = getTexture(path);
    if (!tex) {
        ImGui::TextColored(g_errorColor, "❌ Ошибка загрузки");
        return;
    }
    
    float w = static_cast<float>(tex->width);
    float h = static_cast<float>(tex->height);
    float scale = std::min({maxWidth / w, maxHeight / h, 1.0f});
    
    ImGui::Image(reinterpret_cast<void*>(static_cast<intptr_t>(tex->id)), 
                 ImVec2(w * scale, h * scale));
}

static std::string resolveImagePath(const std::string& raw) {
    if (raw.empty()) return {};

    const std::filesystem::path p(raw);
    if (std::filesystem::exists(p)) return p.string();

    const std::filesystem::path cwd = std::filesystem::current_path();
    if (std::filesystem::exists(cwd / p)) return (cwd / p).string();

    // Common project locations (helps when stored paths are just filenames).
    const std::filesystem::path filename = p.filename();
    const std::filesystem::path dataDir = std::filesystem::exists(cwd / "data") ? (cwd / "data") : std::filesystem::path{};
    const bool hasDirSeparators = raw.find('/') != std::string::npos || raw.find('\\') != std::string::npos;

    auto toLowerAscii = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return s;
    };

    if (!hasDirSeparators && std::filesystem::exists(g_externalImagesDir) && std::filesystem::is_directory(g_externalImagesDir)) {
        // Recursive case-insensitive search by filename in C:/Users/Egor/Desktop/images
        const std::string targetName = toLowerAscii(filename.string());
        const std::filesystem::path direct = g_externalImagesDir / filename;
        if (std::filesystem::exists(direct)) return direct.string();
        for (const auto& entry : std::filesystem::recursive_directory_iterator(g_externalImagesDir)) {
            if (!entry.is_regular_file()) continue;
            if (toLowerAscii(entry.path().filename().string()) == targetName) {
                return entry.path().string();
            }
        }
    }

    const std::array<std::filesystem::path, 7> candidates = {
        (!hasDirSeparators && std::filesystem::exists(g_externalImagesDir)) ? (g_externalImagesDir / filename) : std::filesystem::path{},
        (!g_lastImageDirectory.empty() && !hasDirSeparators) ? (g_lastImageDirectory / filename) : std::filesystem::path{},
        cwd / "images" / filename,
        cwd / "assets" / "images" / filename,
        dataDir.empty() ? std::filesystem::path{} : (dataDir / filename),
        dataDir.empty() ? std::filesystem::path{} : (dataDir / p),
        cwd / filename
    };
    for (const auto& c : candidates) {
        if (!c.empty() && std::filesystem::exists(c)) return c.string();
    }

    return raw;
}

static std::string normalizeUserPath(std::string raw) {
    // Trim spaces
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front()))) raw.erase(raw.begin());
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) raw.pop_back();

    // Remove wrapping quotes if user pasted "C:\...\file.png"
    if (raw.size() >= 2) {
        const char first = raw.front();
        const char last = raw.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            raw = raw.substr(1, raw.size() - 2);
        }
    }

    // Unify separators for easier manual input.
    std::replace(raw.begin(), raw.end(), '\\', '/');
    return raw;
}

static void drawStatusBar(const std::string& status, const std::string& error = "") {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();

    // Подложка статус-бара (чтобы текст был читаемее на фоне dock-окон).
    drawList->AddRectFilled(
        pos,
        ImVec2(pos.x + size.x, pos.y + size.y),
        IM_COL32(18, 18, 26, 210)
    );

    ImGui::SetCursorPosX(12.0f);
    if (!error.empty()) {
        ImGui::TextColored(g_errorColor, "❌ %s", error.c_str());
    } else if (!status.empty()) {
        ImGui::TextColored(g_successColor, "✓ %s", status.c_str());
    }
}

static void collectTreeInOrder(const nlohmann::json& node, std::vector<nlohmann::json>& out) {
    if (node.is_null()) return;
    collectTreeInOrder(node["left"], out);
    out.push_back(node);
    collectTreeInOrder(node["right"], out);
}

static void drawRecordCard(const nlohmann::json& rec, bool showDeleteButton, UiState* ui = nullptr) {
    ImGui::PushID(rec.value("sport_id", 0));
    
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.10f, 0.14f, 0.65f));
    // Compact fixed-height card so each record doesn't occupy whole panel.
    ImGui::BeginChild("##record", ImVec2(0, 400.0f), ImGuiChildFlags_Borders);
    
    // Заголовок карточки
    ImVec4 headerColor = rec.value("olympic_status", false) ? 
                         ImVec4(0.9f, 0.8f, 0.2f, 1.0f) : ImVec4(0.4f, 0.4f, 0.4f, 1.0f);
    ImGui::TextColored(headerColor, " %s", rec.value("name", "").c_str());
    ImGui::SameLine();
    if (rec.value("olympic_status", false)) {
        ImGui::TextColored(g_warningColor, "(Олимпийский вид)");
    }
    
    ImGui::Separator();

    // Быстрая ориентация.
    if (!rec.value("category", "").empty()) {
        ImGui::TextDisabled("Категория: %s", rec.value("category", "").c_str());
    }

    // Изображение (показываем сразу вверху карточки, чтобы было видно в списках).
    const std::string imagePath = resolveImagePath(rec.value("image_path", ""));

    if (ImGui::BeginTable("##record_layout", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("info", ImGuiTableColumnFlags_WidthStretch, 0.70f);
        ImGui::TableSetupColumn("image", ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableNextRow();

        // Левая колонка: основные поля
        ImGui::TableSetColumnIndex(0);
        if (ImGui::BeginTable("##record_info", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("ID:");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%d", rec.value("sport_id", 0));

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Категория:");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", rec.value("category", "").c_str());

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Управляющий орган:");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", rec.value("governing_body", "").c_str());

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Описание:");
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", rec.value("description", "").c_str());

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Противопоказания:");
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", rec.value("medical_contraindications", "").c_str());

            ImGui::EndTable();
        }

        // Правая колонка: превью картинки
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("");
        ImGui::SameLine();
        ImGui::TextDisabled("Фото");
        drawImagePreview(imagePath, 200.0f, 140.0f);

        ImGui::EndTable();
    }
    
    // Кнопка удаления
    if (showDeleteButton && ui) {
        ImGui::Separator();
        if (ImGui::Button("Удалить", ImVec2(-1, 0))) {
            ui->pendingDeleteSportId = rec.value("sport_id", 0);
            ui->deleteConfirmOpen = true;
        }
    }
    
    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::PopID();
}

// ========== API ФУНКЦИИ ==========

static std::string urlEncode(const std::string& s) {
    std::ostringstream encoded;
    encoded.fill('0');
    encoded << std::hex << std::uppercase;
    
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded << c;
        } else {
            encoded << '%' << std::setw(2) << int(c);
        }
    }
    return encoded.str();
}

static bool fetchPage(UiState& ui) {
    httplib::Client cli("127.0.0.1", 8080);
    cli.set_connection_timeout(1, 0);
    
    std::string url = "/api/sports?page=" + std::to_string(ui.page) +
                      "&limit=" + std::to_string(ui.limit);
    
    if (strlen(ui.filterCategory) > 0) {
        url += "&category=" + urlEncode(ui.filterCategory);
    }

    if (ui.filterOlympic) {
        url += "&olympic_status=true";
    } else if (ui.filterNonOlympic) {
        url += "&olympic_status=false";
    }
    
    auto res = cli.Get(url.c_str());
    
    if (!res) {
        ui.errorMessage = "Не удалось подключиться к серверу";
        return false;
    }
    
    if (res->status != 200) {
        ui.errorMessage = "Ошибка сервера: " + res->body;
        return false;
    }
    
    try {
        auto json = nlohmann::json::parse(res->body);
        
        if (json["status"] != "success") {
            ui.errorMessage = "Ошибка API";
            return false;
        }
        
        ui.total = json["data"]["total"].get<int>();
        ui.records.clear();
        
        for (auto& rec : json["data"]["records"]) {
            ui.records.push_back(rec);
        }
        
        int maxPage = std::max(1, (ui.total + ui.limit - 1) / ui.limit);
        if (ui.page > maxPage) ui.page = maxPage;
        
        ui.status = "Загружено " + std::to_string(ui.records.size()) + " записей";
        ui.errorMessage.clear();
        return true;
        
    } catch (const std::exception& e) {
        ui.errorMessage = "Ошибка парсинга JSON";
        return false;
    }
}

static bool fetchCategories(UiState& ui) {
    httplib::Client cli("127.0.0.1", 8080);
    cli.set_connection_timeout(1, 0);
    auto res = cli.Get("/api/sports/categories");

    if (!res || res->status != 200) {
        return false;
    }

    try {
        auto json = nlohmann::json::parse(res->body);
        if (!json.contains("categories") || !json["categories"].is_array()) return false;

        ui.knownCategories.clear();
        ui.knownCategories.reserve(json["categories"].size());
        for (const auto& c : json["categories"]) {
            if (c.is_string()) ui.knownCategories.push_back(c.get<std::string>());
        }
        return true;
    } catch (...) {
        return false;
    }
}

static void doSort(UiState& ui) {
    httplib::Client cli("127.0.0.1", 8080);
    std::string url = "/api/sports/sort?field=" + std::string(ui.sortField) +
                      "&order=" + std::string(ui.sortAscending ? "asc" : "desc");
    
    auto res = cli.Post(url.c_str(), "", "application/json");
    
    if (!res) {
        ui.errorMessage = "Ошибка сортировки";
        return;
    }
    
    ui.status = std::string("Сортировка выполнена (") + (ui.sortAscending ? "ASC" : "DESC") + ")";
    fetchPage(ui);
}

static void doBinarySearch(UiState& ui) {
    if (strlen(ui.searchName) == 0) {
        ui.errorMessage = "Введите имя для поиска";
        return;
    }
    
    httplib::Client cli("127.0.0.1", 8080);
    std::string encodedName = urlEncode(std::string(ui.searchName));
    auto res = cli.Get(("/api/sports/search?name=" + encodedName).c_str());
    
    if (!res) {
        ui.hasBinarySearchRecord = false;
        ui.errorMessage = "Сервер недоступен";
        return;
    }
    if (res->status != 200) {
        ui.hasBinarySearchRecord = false;
        ui.errorMessage = "Ошибка поиска";
        try {
            auto err = nlohmann::json::parse(res->body);
            if (err.contains("message") && err["message"].is_string()) {
                ui.errorMessage = err["message"].get<std::string>();
            }
        } catch (...) {
            if (res->status == 404) ui.errorMessage = "Запись не найдена";
        }
        return;
    }
    
    try {
        auto json = nlohmann::json::parse(res->body);
        ui.binarySearchRecord = json["record"];
        ui.hasBinarySearchRecord = true;
        ui.status = "Найдена запись: " + json["record"]["name"].get<std::string>();
        ui.errorMessage.clear();
    } catch (const std::exception& e) {
        ui.hasBinarySearchRecord = false;
        ui.errorMessage = "Ошибка поиска";
    }
}

static void addRecord(UiState& ui) {
    if (strlen(ui.sportId) == 0 || strlen(ui.name) == 0 || strlen(ui.category) == 0) {
        ui.errorMessage = "Заполните ID, название и категорию";
        return;
    }
    // Простейшая валидация ID: число > 0.
    for (const char* p = ui.sportId; *p; ++p) {
        if (!std::isdigit(static_cast<unsigned char>(*p))) {
            ui.errorMessage = "ID должен содержать только цифры";
            return;
        }
    }
    
    httplib::Client cli("127.0.0.1", 8080);
    int id = std::atoi(ui.sportId);
    if (id <= 0) {
        ui.errorMessage = "ID должен быть положительным числом";
        return;
    }

    // Такой же принцип, как у предзаполненных записей:
    // сохраняем корректный image_path (предпочтительно имя файла), а отображение
    // выполняется через resolveImagePath с поиском в стандартных директориях.
    std::string finalImagePath;
    const std::string typedPath = normalizeUserPath(std::string(ui.imagePath));
    const std::filesystem::path selectedPath(ui.selectedImageSourcePath);

    if (!ui.selectedImageSourcePath.empty() && std::filesystem::exists(selectedPath)) {
        finalImagePath = selectedPath.filename().string(); // как у стартовых записей (например football.png)
        if (selectedPath.has_parent_path()) g_lastImageDirectory = selectedPath.parent_path();
    } else if (!typedPath.empty()) {
        const std::string resolvedTyped = resolveImagePath(typedPath);
        if (!resolvedTyped.empty() && std::filesystem::exists(std::filesystem::path(resolvedTyped))) {
            finalImagePath = std::filesystem::path(resolvedTyped).filename().string();
            const std::filesystem::path resolvedPath(resolvedTyped);
            if (resolvedPath.has_parent_path()) g_lastImageDirectory = resolvedPath.parent_path();
        } else {
            // Если ввели только имя файла, сохраняем как есть — resolveImagePath попробует
            // найти файл при показе (в т.ч. C:/Users/Egor/Desktop/images).
            finalImagePath = typedPath;
        }
    }
    
    nlohmann::json body = {
        {"sport_id", id},
        {"name", std::string(ui.name)},
        {"category", std::string(ui.category)},
        {"olympic_status", ui.olympic},
        {"description", std::string(ui.description)},
        {"governing_body", std::string(ui.governingBody)},
        {"image_path", finalImagePath},
        {"medical_contraindications", std::string(ui.contraindications)}
    };
    
    auto res = cli.Post("/api/sports", body.dump(), "application/json");
    
    if (res && res->status == 200) {
        ui.status = "Запись добавлена";
        ui.errorMessage.clear();
        fetchPage(ui);
        fetchCategories(ui);
        
        // Очистка формы
        memset(ui.sportId, 0, sizeof(ui.sportId));
        memset(ui.name, 0, sizeof(ui.name));
        memset(ui.category, 0, sizeof(ui.category));
        memset(ui.description, 0, sizeof(ui.description));
        memset(ui.governingBody, 0, sizeof(ui.governingBody));
        memset(ui.imagePath, 0, sizeof(ui.imagePath));
        memset(ui.contraindications, 0, sizeof(ui.contraindications));
        ui.olympic = false;
        ui.selectedImageSourcePath.clear();
        ui.imagePickMessage.clear();
    } else {
        ui.errorMessage = "Ошибка добавления записи";
    }
}

static void deleteRecord(UiState& ui, int id) {
    httplib::Client cli("127.0.0.1", 8080);
    auto res = cli.Post(("/api/sports/delete?id=" + std::to_string(id)).c_str(), "", "application/json");
    
    if (res && res->status == 200) {
        ui.status = "Запись удалена";
        fetchPage(ui);
        fetchCategories(ui);
    } else {
        ui.errorMessage = "Ошибка удаления";
    }
}

// ========== ОСНОВНАЯ ФУНКЦИЯ ==========

int main() {
    // Настройка рабочей директории
    const ResolvedPaths paths = setupWorkingDirectory();
    
    // Создание директорий
    const std::filesystem::path dataDir = paths.dataRoot.empty() ? std::filesystem::current_path() / "data" : paths.dataRoot / "data";
    std::filesystem::create_directories(dataDir);
    std::filesystem::create_directories("logs");
    std::filesystem::create_directories("assets/images");
    std::filesystem::create_directories("assets/fonts");
    
    // Инициализация сервера
    Logger logger("logs/app.log");
    Storage storage(
        (dataDir / "sports_database.dat").string(),
        (dataDir / "sports_database_backup.dat").string(),
        logger
    );
    LocalApiServer server(storage, logger);
    server.start();
    
    std::cout << "Server started on http://127.0.0.1:8080" << std::endl;
    
    // Инициализация GLFW
    if (!glfwInit()) {
        std::cerr << "GLFW init failed\n";
        return 1;
    }
    
    // Настройка GLFW для лучшего внешнего вида
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    
    GLFWwindow* window = glfwCreateWindow(1600, 900, "Sports Directory Manager", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    
    // Инициализация ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    
    // Настройка стиля
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 12.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.FramePadding = ImVec2(8.0f, 5.0f);
    style.ItemSpacing = ImVec2(10.0f, 6.0f);

    // Цвета для лучшей читаемости.
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.08f, 0.94f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.07f, 0.07f, 0.10f, 0.65f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.22f, 0.22f, 0.30f, 0.55f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.18f, 0.32f, 0.80f);

    style.Colors[ImGuiCol_Button] = ImVec4(g_primaryColor.x, g_primaryColor.y, g_primaryColor.z, 0.65f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(g_primaryColor.x, g_primaryColor.y, g_primaryColor.z, 0.85f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.08f, 0.26f, 0.52f, 1.0f);
    
    // Загрузка шрифта
    std::string fontPath = "assets/fonts/DejaVuSans.ttf";
    if (std::filesystem::exists(fontPath)) {
        ImFontConfig font_cfg;
        font_cfg.OversampleH = 2;
        font_cfg.OversampleV = 2;
        io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 18.0f, &font_cfg, io.Fonts->GetGlyphRangesCyrillic());
    } else {
        io.Fonts->AddFontDefault();
        std::cerr << "Font not found, using default" << std::endl;
    }
    
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");
    
    // Инициализация состояния
    UiState ui;
    fetchPage(ui);
    fetchCategories(ui);
    
    // Главный цикл
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        
        // Docking space
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        
        // ========== ВЕРХНЯЯ ПАНЕЛЬ ==========
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("Файл")) {
                if (ImGui::MenuItem("Обновить данные", "F5")) {
                    fetchPage(ui);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Выход", "Alt+F4")) {
                    glfwSetWindowShouldClose(window, true);
                }
                ImGui::EndMenu();
            }
            
            if (ImGui::BeginMenu("Вид")) {
                ImGui::MenuItem("Форма добавления", nullptr, &ui.showAddForm);
                ImGui::MenuItem("Список записей", nullptr, &ui.showRecords);
                ImGui::MenuItem("Дерево категорий", nullptr, &ui.showTree);
                ImGui::MenuItem("Поиск", nullptr, &ui.showSearch);
                ImGui::MenuItem("Статистика", nullptr, &ui.showStats);
                ImGui::EndMenu();
            }
            
            ImGui::EndMainMenuBar();
        }
        
        // ========== СТАТУС БАР ==========
        ImGui::SetNextWindowPos(ImVec2(0, ImGui::GetFrameHeight()));
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x, 30));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
        if (ImGui::Begin("##statusbar", nullptr, 
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar)) {
            drawStatusBar(ui.status, ui.errorMessage);
        }
        ImGui::End();
        ImGui::PopStyleVar();
        
        // ========== ОДНО ГЛАВНОЕ ОКНО ==========
        ImGui::SetNextWindowSize(ImVec2(1200, 850), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(" Sports Directory Manager")) {
            ImGui::Columns(2, "main_layout", true);
            ImGui::SetColumnWidth(0, 380.0f);

            ImGui::BeginChild("##left_panel", ImVec2(0, 0), true);

            ImGui::Text("Страница %d из %d", ui.page, std::max(1, (ui.total + ui.limit - 1) / ui.limit));
            ImGui::Text("Всего записей: %d", ui.total);

            if (ImGui::Button("◀ Назад")) {
                if (ui.page > 1) {
                    ui.page--;
                    fetchPage(ui);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Вперед ▶")) {
                int maxPage = std::max(1, (ui.total + ui.limit - 1) / ui.limit);
                if (ui.page < maxPage) {
                    ui.page++;
                    fetchPage(ui);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Обновить")) {
                fetchPage(ui);
            }

            ImGui::Separator();

            ImGui::Text("Фильтры:");
            if (!ui.knownCategories.empty()) {
                ImGui::TextDisabled("Категория (выбор):");

                if (ImGui::Button("Все", ImVec2(70, 0))) {
                    memset(ui.filterCategory, 0, sizeof(ui.filterCategory));
                    ui.page = 1;
                    fetchPage(ui);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%s", (strlen(ui.filterCategory) > 0) ? ui.filterCategory : "Все категории");

                const float avail = ImGui::GetContentRegionAvail().x;
                float x = 0.0f;
                for (const auto& cat : ui.knownCategories) {
                    const std::string label = cat;
                    const float w = ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                    if (x > 0.0f && (x + w) > avail) {
                        x = 0.0f;
                    }
                    if (x > 0.0f) ImGui::SameLine();

                    const bool active = (strlen(ui.filterCategory) > 0 && label == ui.filterCategory);
                    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(g_primaryColor.x, g_primaryColor.y, g_primaryColor.z, 0.95f));
                    if (ImGui::Button(label.c_str())) {
                        std::strncpy(ui.filterCategory, label.c_str(), sizeof(ui.filterCategory));
                        ui.filterCategory[sizeof(ui.filterCategory) - 1] = '\0';
                        ui.page = 1;
                        fetchPage(ui);
                    }
                    if (active) ImGui::PopStyleColor();

                    x += w + ImGui::GetStyle().ItemSpacing.x;
                }

                ImGui::Separator();
            } else {
                ImGui::TextDisabled("Категории не найдены (нет данных или сервер не ответил).");
            }

            ImGui::Checkbox("Только олимпийские", &ui.filterOlympic);
            if (ui.filterOlympic) ui.filterNonOlympic = false;
            ImGui::Checkbox("Только неолимпийские", &ui.filterNonOlympic);
            if (ui.filterNonOlympic) ui.filterOlympic = false;
            if (ImGui::Button("Применить фильтр")) {
                ui.page = 1;
                fetchPage(ui);
            }
            ImGui::SameLine();
            if (ImGui::Button("Сбросить")) {
                memset(ui.filterCategory, 0, sizeof(ui.filterCategory));
                ui.filterOlympic = false;
                ui.filterNonOlympic = false;
                ui.page = 1;
                fetchPage(ui);
            }

            ImGui::Separator();

            ImGui::Text("Сортировка:");
            const SortField currentSort = sortFieldFromString(std::string(ui.sortField));
            if (ImGui::BeginCombo("Поле", sortFieldToHuman(currentSort), 0)) {
                constexpr std::array<SortField, 8> options = {
                    SortField::Id,
                    SortField::Name,
                    SortField::Category,
                    SortField::OlympicStatus,
                    SortField::Description,
                    SortField::GoverningBody,
                    SortField::ImagePath,
                    SortField::MedicalContraindications
                };
                for (SortField field : options) {
                    const bool selected = (currentSort == field);
                    if (ImGui::Selectable(sortFieldToHuman(field), selected)) {
                        setSortFieldInUi(ui, field);
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::Checkbox("По порядку", &ui.sortAscending);
            if (ImGui::Button("Сортировать", ImVec2(-1, 0))) {
                doSort(ui);
            }

            ImGui::Separator();

            ImGui::EndChild();

            ImGui::NextColumn();

            ImGui::BeginChild("##right_panel", ImVec2(0, 0), true);
            const bool hasAnyTab = ui.showAddForm || ui.showRecords || ui.showTree || ui.showStats || ui.showSearch;
            if (!hasAnyTab) {
                ImGui::TextDisabled("Включите хотя бы один раздел в меню `Вид`.");
            } else if (ImGui::BeginTabBar("MainTabs")) {
                if (ui.showRecords) {
                    if (ImGui::BeginTabItem("Список записей")) {
                        ImGui::Text("Найдено записей: %zu", ui.records.size());
                        ImGui::Separator();
                        ImGui::BeginChild("##records_list", ImVec2(0, 0), true);
                        if (ui.records.empty()) {
                            ImGui::TextDisabled("Пока нет данных. Нажмите `Обновить` или измените фильтры.");
                        } else {
                            for (const auto& rec : ui.records) {
                                drawRecordCard(rec, true, &ui);
                            }
                        }
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }
                }

                if (ui.showAddForm) {
                    if (ImGui::BeginTabItem("Добавление записи")) {
                        ImGui::BeginChild("##add_record_tab", ImVec2(0, 0), true);
                        ImGui::InputText("ID (число)*", ui.sportId, sizeof(ui.sportId));
                        ImGui::InputText("Название*", ui.name, sizeof(ui.name));
                        ImGui::InputText("Категория*", ui.category, sizeof(ui.category));
                        ImGui::Checkbox("Олимпийский вид", &ui.olympic);
                        ImGui::InputTextMultiline("Описание", ui.description, sizeof(ui.description), ImVec2(-1, 120));
                        ImGui::InputText("Управляющий орган", ui.governingBody, sizeof(ui.governingBody));
                        ImGui::InputText("Противопоказания", ui.contraindications, sizeof(ui.contraindications));

                        ImGui::Separator();
                        ImGui::Text("Изображение:");
                        ImGui::InputTextWithHint(
                            "Путь к файлу",
                            "Пример: C:/Users/Egor/Pictures/football.png",
                            ui.imagePath,
                            sizeof(ui.imagePath)
                        );
                        ImGui::TextDisabled("");
                        if (ImGui::Button("Выбрать файл")) {
                            ui.imagePickerOpen = true;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Очистить выбор")) {
                            ui.selectedImageSourcePath.clear();
                            ui.imagePath[0] = '\0';
                            ui.imagePickMessage.clear();
                        }

                        if (!ui.selectedImageSourcePath.empty()) {
                            const bool okFile = std::filesystem::exists(std::filesystem::path(ui.selectedImageSourcePath));
                            ImGui::TextColored(okFile ? g_successColor : g_warningColor,
                                               "%s: %s",
                                               okFile ? "Выбран" : "Проблема с файлом",
                                               ui.selectedImageSourcePath.c_str());
                            if (!ui.imagePickMessage.empty()) {
                                ImGui::TextDisabled("%s", ui.imagePickMessage.c_str());
                            }
                            ImGui::Text("Предпросмотр:");
                            drawImagePreview(resolveImagePath(ui.selectedImageSourcePath), 170.0f, 120.0f);
                        }

                        ImGui::Separator();
                        if (ImGui::Button("Добавить запись", ImVec2(-1, 0))) {
                            addRecord(ui);
                        }
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }
                }

                if (ui.showTree) {
                    if (ImGui::BeginTabItem("Дерево категорий")) {
                        if (ImGui::BeginTabBar("TreeTabs")) {
                            for (size_t i = 0; i < g_treeFields.size(); ++i) {
                                SortField field = g_treeFields[i];
                                const char* label = g_treeLabels[i];
                                TreeTabState& tab = ui.treeTabs[i];

                                if (ImGui::BeginTabItem(label)) {
                                    if (ImGui::Button("Построить дерево")) {
                                        httplib::Client cli("127.0.0.1", 8080);
                                        std::string fieldStr = sortFieldToBackendString(field);
                                        auto res = cli.Get(("/api/sports/tree?field=" + fieldStr).c_str());

                                        if (res && res->status == 200) {
                                            auto json = nlohmann::json::parse(res->body);
                                            tab.tree = json["tree"];
                                            tab.status = "Дерево построено";
                                        } else {
                                            tab.status = "Ошибка построения дерева";
                                        }
                                    }

                                    ImGui::SameLine();
                                    ImGui::InputText("Поиск в дереве", tab.searchValue, sizeof(tab.searchValue));
                                    ImGui::SameLine();
                                    if (ImGui::Button("Найти")) {
                                        httplib::Client cli("127.0.0.1", 8080);
                                        std::string fieldStr = sortFieldToBackendString(field);
                                        std::string value = tab.searchValue;
                                        std::string encodedValue = urlEncode(value);
                                        auto res = cli.Get(("/api/sports/tree/search?field=" + fieldStr + "&value=" + encodedValue).c_str());

                                        if (res && res->status == 200) {
                                            auto json = nlohmann::json::parse(res->body);
                                            if (json.contains("record")) {
                                                tab.foundRecord = json["record"];
                                                tab.hasFoundRecord = true;
                                                tab.status = "Найдено: " + json["record"]["name"].get<std::string>();
                                            } else {
                                                tab.hasFoundRecord = false;
                                                tab.status = "Не найдено";
                                            }
                                        }
                                    }

                                    ImGui::Separator();
                                    ImGui::Text("Статус: %s", tab.status.c_str());
                                    ImGui::Separator();

                                    if (!tab.tree.is_null()) {
                                        std::vector<nlohmann::json> nodes;
                                        nodes.reserve(256);
                                        collectTreeInOrder(tab.tree, nodes);

                                        ImGui::BeginChild(("##treeview_" + std::string(label)).c_str(), ImVec2(0, 260), true);

                                        ImGui::Text("Обход слева направо:");
                                        if (nodes.empty()) {
                                            ImGui::TextDisabled("Дерево пусто.");
                                        } else {
                                            int idx = 1;
                                            for (const auto& n : nodes) {
                                                const std::string display = n.value("display_value", "");
                                                const int sid = n.value("sport_id", 0);
                                                const int w = n.value("weight", 0);
                                                ImGui::Text("%d) %s  [id=%d, w=%d]", idx++, display.c_str(), sid, w);
                                            }
                                        }

                                        ImGui::Separator();
                                        ImGui::Text("Обход справа налево:");
                                        if (nodes.empty()) {
                                            ImGui::TextDisabled("Дерево пусто.");
                                        } else {
                                            int idx = 1;
                                            for (auto it = nodes.rbegin(); it != nodes.rend(); ++it, ++idx) {
                                                const auto& n = *it;
                                                const std::string display = n.value("display_value", "");
                                                const int sid = n.value("sport_id", 0);
                                                const int w = n.value("weight", 0);
                                                ImGui::Text("%d) %s  [id=%d, w=%d]", idx, display.c_str(), sid, w);
                                            }
                                        }

                                        ImGui::EndChild();
                                    } else {
                                        ImGui::Text("Нажмите 'Построить дерево'");
                                    }

                                    ImGui::Separator();
                                    if (tab.hasFoundRecord) {
                                        ImGui::Text("Найденная запись:");
                                        drawRecordCard(tab.foundRecord, false);
                                    }

                                    ImGui::EndTabItem();
                                }
                            }
                            ImGui::EndTabBar();
                        }
                        ImGui::EndTabItem();
                    }
                }

                if (ui.showStats) {
                    if (ImGui::BeginTabItem(" Статистика")) {
                        std::unordered_map<std::string, int> categoryCount;
                        int olympicCount = 0;
                        for (const auto& rec : ui.records) {
                            std::string cat = rec.value("category", "Другое");
                            categoryCount[cat]++;
                            if (rec.value("olympic_status", false)) olympicCount++;
                        }

                        ImGui::Text(" Общая статистика:");
                        ImGui::Text("  • Всего записей: %d", ui.total);
                        ImGui::Text("  • Отображается: %zu", ui.records.size());
                        ImGui::Text("  • Олимпийских видов: %d", olympicCount);

                        ImGui::Separator();
                        ImGui::Text(" По категориям:");
                        for (const auto& [cat, count] : categoryCount) {
                            ImGui::Text("  • %s: %d", cat.c_str(), count);
                        }

                        ImGui::EndTabItem();
                    }
                }

                if (ui.showSearch) {
                    if (ImGui::BeginTabItem("Поиск")) {
                        ImGui::Text("Бинарный поиск:");
                        ImGui::InputText("Имя", ui.searchName, sizeof(ui.searchName));
                        if (ImGui::Button("Найти")) {
                            doBinarySearch(ui);
                        }
                        ImGui::Separator();

                        if (ui.hasBinarySearchRecord) {
                            drawRecordCard(ui.binarySearchRecord, false);
                        } else {
                            ImGui::TextDisabled("");
                        }
                        ImGui::EndTabItem();
                    }
                }

                ImGui::EndTabBar();
            }

            ImGui::EndChild();

            ImGui::Columns(1);
            ImGui::End();
        }
        
        // ========== ПОДТВЕРЖДЕНИЕ УДАЛЕНИЯ ==========
        if (ui.deleteConfirmOpen) {
            ImGui::OpenPopup("ConfirmDeleteSport");
        }
        if (ImGui::BeginPopupModal("ConfirmDeleteSport", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Удалить запись с ID %d?", ui.pendingDeleteSportId);
            ImGui::Separator();

            ImGui::PushStyleColor(ImGuiCol_Button, g_errorColor);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(g_errorColor.x, g_errorColor.y, g_errorColor.z, 0.92f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.65f, 0.10f, 0.10f, 1.0f));

            if (ImGui::Button("Да, удалить", ImVec2(160, 0))) {
                const int id = ui.pendingDeleteSportId;
                ui.deleteConfirmOpen = false;
                ui.pendingDeleteSportId = 0;
                deleteRecord(ui, id);
                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine();

            if (ImGui::Button("Отмена", ImVec2(120, 0))) {
                ui.deleteConfirmOpen = false;
                ui.pendingDeleteSportId = 0;
                ImGui::CloseCurrentPopup();
            }

            ImGui::PopStyleColor(3);
            ImGui::EndPopup();
        }

        // ========== ВСТРОЕННЫЙ ВЫБОР ФАЙЛА ==========
        if (ui.imagePickerOpen) {
            ImGui::OpenPopup("ImagePickerModal");
        }
        if (ImGui::BeginPopupModal("ImagePickerModal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Выбор изображения для новой записи");
            ImGui::Separator();
            ImGui::InputText("Папка", ui.imagePickerDir, sizeof(ui.imagePickerDir));
            ImGui::TextDisabled("Допустимые форматы: .png .jpg .jpeg .bmp .tga");
            ImGui::Separator();

            auto isAllowedImage = [](const std::filesystem::path& p) {
                std::string ext = p.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga";
            };

            std::vector<std::filesystem::path> files;
            std::string pickerError;
            try {
                const std::filesystem::path dir(ui.imagePickerDir);
                if (std::filesystem::exists(dir) && std::filesystem::is_directory(dir)) {
                    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                        if (!entry.is_regular_file()) continue;
                        if (isAllowedImage(entry.path())) files.push_back(entry.path());
                    }
                    std::sort(files.begin(), files.end());
                } else {
                    pickerError = "Папка не существует";
                }
            } catch (...) {
                pickerError = "Не удалось прочитать папку";
            }

            ImGui::BeginChild("##image_picker_list", ImVec2(560, 260), true);
            if (!pickerError.empty()) {
                ImGui::TextColored(g_errorColor, "%s", pickerError.c_str());
            } else if (files.empty()) {
                ImGui::TextDisabled("В этой папке нет подходящих изображений.");
            } else {
                for (const auto& p : files) {
                    const std::string label = p.filename().string();
                    if (ImGui::Selectable(label.c_str())) {
                        ui.selectedImageSourcePath = p.string();
                        std::strncpy(ui.imagePath, ui.selectedImageSourcePath.c_str(), sizeof(ui.imagePath));
                        ui.imagePath[sizeof(ui.imagePath) - 1] = '\0';
                        ui.imagePickMessage = "Файл выбран";
                        g_lastImageDirectory = p.parent_path();
                        ui.imagePickerOpen = false;
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
            ImGui::EndChild();

            if (ImGui::Button("Отмена", ImVec2(120, 0))) {
                ui.imagePickerOpen = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        
        // Рендеринг
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.08f, 0.08f, 0.10f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            GLFWwindow* backup_current_context = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(backup_current_context);
        }
        
        glfwSwapBuffers(window);
    }
    
    // Очистка
    destroyAllTextures();
    ImPlot::DestroyContext();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    
    return 0;
}