#include "models.h"
#include "logger.h"
#include "storage.h"
#include "server.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "portable-file-dialogs.h"

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
#include <chrono>
#include <thread>

// ========== ПРОТОТИПЫ ФУНКЦИЙ ==========
// Forward declaration to allow function prototypes to use UiState before its definition.
struct UiState;
static void deleteRecord(UiState& ui, int id);
static void drawTreeTextInOrder(const nlohmann::json& node, int depth = 0);
static std::string sortFieldToBackendString(SortField field);

// ========== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ==========

// Получение пути к исполняемому файлу
static std::filesystem::path getExecutablePath() {
    char buffer[1024];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer)-1);
    if (len != -1) {
        buffer[len] = '\0';
        return std::filesystem::path(buffer).parent_path();
    }
    return std::filesystem::current_path();
}

// Настройка рабочей директории
static void setupWorkingDirectory() {
    std::filesystem::path exePath = getExecutablePath();
    
    // Если программа запущена из build, поднимаемся на уровень выше
    if (exePath.filename() == "build") {
        exePath = exePath.parent_path();
        std::filesystem::current_path(exePath);
    }
    
    std::cout << "Working directory: " << std::filesystem::current_path() << std::endl;
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
    bool isBuilding = false;
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
    std::string sortOrder = "ASC";
    
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
    
    // Статистика
    int totalOlympic = 0;
    int totalCategories = 0;
    std::unordered_map<std::string, int> categoryStats;
};

// ========== ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ==========

static std::unordered_map<std::string, TextureData> g_textureCache;
static ImVec4 g_successColor = ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
static ImVec4 g_errorColor = ImVec4(0.9f, 0.2f, 0.2f, 1.0f);
static ImVec4 g_warningColor = ImVec4(0.9f, 0.6f, 0.1f, 1.0f);
static ImVec4 g_primaryColor = ImVec4(0.2f, 0.5f, 0.9f, 1.0f);

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

static void drawTreeTextInOrder(const nlohmann::json& node, int depth) {
    if (node.is_null()) return;

    drawTreeTextInOrder(node["left"], depth + 1);

    std::string prefix(depth * 4, ' ');
    std::string line = prefix +
        node["display_value"].get<std::string>() +
        " [id=" + std::to_string(node["sport_id"].get<int>()) + "]" +
        " (w:" + std::to_string(node["weight"].get<int>()) + ")";

    ImGui::TextUnformatted(line.c_str());

    drawTreeTextInOrder(node["right"], depth + 1);
}

static void drawRecordCard(const nlohmann::json& rec, bool showDeleteButton, UiState* ui = nullptr) {
    ImGui::PushID(rec.value("sport_id", 0));
    
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.10f, 0.14f, 0.65f));
    ImGui::BeginChild("##record", ImVec2(0, 0), ImGuiChildFlags_Borders);
    
    // Заголовок карточки
    ImVec4 headerColor = rec.value("olympic_status", false) ? 
                         ImVec4(0.9f, 0.8f, 0.2f, 1.0f) : ImVec4(0.4f, 0.4f, 0.4f, 1.0f);
    ImGui::TextColored(headerColor, "🏅 %s", rec.value("name", "").c_str());
    ImGui::SameLine();
    if (rec.value("olympic_status", false)) {
        ImGui::TextColored(g_warningColor, "(Олимпийский вид)");
    }
    
    ImGui::Separator();

    // Быстрая ориентация.
    if (!rec.value("category", "").empty()) {
        ImGui::TextDisabled("Категория: %s", rec.value("category", "").c_str());
    }
    
    // Основная информация в две колонки
    if (ImGui::BeginTable("##record_info", 2, ImGuiTableFlags_Borders)) {
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
    
    // Изображение
    std::string imagePath = rec.value("image_path", "");
    if (!imagePath.empty()) {
        ImGui::Separator();
        ImGui::Text("📸 Изображение:");
        drawImagePreview(imagePath);
    }
    
    // Кнопка удаления
    if (showDeleteButton && ui) {
        ImGui::Separator();
        if (ImGui::Button("🗑 Удалить", ImVec2(-1, 0))) {
            ui->pendingDeleteSportId = rec.value("sport_id", 0);
            ui->deleteConfirmOpen = true;
        }
    }
    
    ImGui::EndChild();
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

static void doSort(UiState& ui) {
    httplib::Client cli("127.0.0.1", 8080);
    std::string url = "/api/sports/sort?field=" + std::string(ui.sortField) +
                      "&order=" + ui.sortOrder;
    
    auto res = cli.Post(url.c_str(), "", "application/json");
    
    if (!res) {
        ui.errorMessage = "Ошибка сортировки";
        return;
    }
    
    ui.status = "Сортировка выполнена";
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
    
    if (!res || res->status != 200) {
        ui.hasBinarySearchRecord = false;
        ui.errorMessage = "Запись не найдена";
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
    if (strlen(ui.sportId) == 0 || strlen(ui.name) == 0) {
        ui.errorMessage = "Заполните ID и название";
        return;
    }
    
    httplib::Client cli("127.0.0.1", 8080);
    int id = std::atoi(ui.sportId);
    std::string finalImagePath;
    
    if (!ui.selectedImageSourcePath.empty()) {
        std::filesystem::create_directories("assets/images");
        std::filesystem::path src(ui.selectedImageSourcePath);
        std::string ext = src.extension().string();
        if (ext.empty()) ext = ".png";
        
        std::string targetName = "sport_" + std::to_string(id) + ext;
        std::filesystem::path dst = std::filesystem::path("assets/images") / targetName;
        
        std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing);
        finalImagePath = dst.string();
    }
    
    if (finalImagePath.empty() && strlen(ui.imagePath) > 0) {
        finalImagePath = ui.imagePath;
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
    } else {
        ui.errorMessage = "Ошибка удаления";
    }
}

// ========== ОСНОВНАЯ ФУНКЦИЯ ==========

int main() {
    // Настройка рабочей директории
    setupWorkingDirectory();
    
    // Создание директорий
    std::filesystem::create_directories("data");
    std::filesystem::create_directories("logs");
    std::filesystem::create_directories("assets/images");
    std::filesystem::create_directories("assets/fonts");
    
    // Инициализация сервера
    Logger logger("logs/app.log");
    Storage storage("data/sports_database.dat", "data/sports_database_backup.dat", logger);
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
        if (ImGui::Begin("📦 Sports Directory Manager")) {
            ImGui::Columns(2, "main_layout", true);
            ImGui::SetColumnWidth(0, 380.0f);

            ImGui::BeginChild("##left_panel", ImVec2(0, 0), true);

            ImGui::Text("📄 Страница %d из %d", ui.page, std::max(1, (ui.total + ui.limit - 1) / ui.limit));
            ImGui::Text("📊 Всего записей: %d", ui.total);

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
            if (ImGui::Button("🔄 Обновить")) {
                fetchPage(ui);
            }

            ImGui::Separator();

            ImGui::Text("🔍 Фильтры:");
            ImGui::InputText("Категория", ui.filterCategory, sizeof(ui.filterCategory));
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

            ImGui::Text("📋 Сортировка:");
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

            if (ImGui::Button("↑ ASC")) {
                ui.sortOrder = "ASC";
                doSort(ui);
            }
            ImGui::SameLine();
            if (ImGui::Button("↓ DESC")) {
                ui.sortOrder = "DESC";
                doSort(ui);
            }

            ImGui::Separator();

            ImGui::Text("🔎 Бинарный поиск:");
            ImGui::InputText("Имя", ui.searchName, sizeof(ui.searchName));
            if (ImGui::Button("Найти")) {
                doBinarySearch(ui);
            }

            ImGui::Separator();

            if (ui.showAddForm) {
                if (ImGui::CollapsingHeader("➕ Добавление записи", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::InputText("ID (число)*", ui.sportId, sizeof(ui.sportId));
                    ImGui::InputText("Название*", ui.name, sizeof(ui.name));
                    ImGui::InputText("Категория", ui.category, sizeof(ui.category));
                    ImGui::Checkbox("Олимпийский вид", &ui.olympic);
                    ImGui::InputTextMultiline("Описание", ui.description, sizeof(ui.description), ImVec2(-1, 80));
                    ImGui::InputText("Управляющий орган", ui.governingBody, sizeof(ui.governingBody));
                    ImGui::InputText("Противопоказания", ui.contraindications, sizeof(ui.contraindications));

                    ImGui::Separator();
                    ImGui::Text("📸 Изображение:");
                    ImGui::InputText("Путь к файлу", ui.imagePath, sizeof(ui.imagePath));
                    if (ImGui::Button("Выбрать файл")) {
                        auto selection = pfd::open_file("Выбор изображения", ".", {"Image Files", "*.png *.jpg *.jpeg *.bmp *.tga"}).result();
                        if (!selection.empty()) {
                            ui.selectedImageSourcePath = selection[0];
                            strncpy(ui.imagePath, selection[0].c_str(), sizeof(ui.imagePath));
                        }
                    }

                    if (!ui.selectedImageSourcePath.empty()) {
                        ImGui::TextColored(g_successColor, "✓ Выбран: %s", ui.selectedImageSourcePath.c_str());
                    }

                    ImGui::Separator();
                    if (ImGui::Button("✅ Добавить запись", ImVec2(-1, 0))) {
                        addRecord(ui);
                    }
                }
            }

            ImGui::EndChild();

            ImGui::NextColumn();

            ImGui::BeginChild("##right_panel", ImVec2(0, 0), true);
            const bool hasAnyTab = ui.showRecords || ui.showTree || ui.showStats || ui.showSearch;
            if (!hasAnyTab) {
                ImGui::TextDisabled("Включите хотя бы один раздел в меню `Вид`.");
            } else if (ImGui::BeginTabBar("MainTabs")) {
                if (ui.showRecords) {
                    if (ImGui::BeginTabItem("📋 Список записей")) {
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

                if (ui.showTree) {
                    if (ImGui::BeginTabItem("🌲 Дерево категорий")) {
                        if (ImGui::BeginTabBar("TreeTabs")) {
                            for (size_t i = 0; i < g_treeFields.size(); ++i) {
                                SortField field = g_treeFields[i];
                                const char* label = g_treeLabels[i];
                                TreeTabState& tab = ui.treeTabs[i];

                                if (ImGui::BeginTabItem(label)) {
                                    if (ImGui::Button("Построить дерево")) {
                                        tab.isBuilding = true;
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
                                        tab.isBuilding = false;
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

                                    if (!tab.tree.is_null() && !tab.isBuilding) {
                                        ImGui::Text("Обход дерева (in-order):");
                                        ImGui::BeginChild(("##treeview_" + std::string(label)).c_str(), ImVec2(0, 300), true);
                                        drawTreeTextInOrder(tab.tree);
                                        ImGui::EndChild();
                                    } else if (tab.isBuilding) {
                                        ImGui::Text("Построение дерева...");
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
                    if (ImGui::BeginTabItem("📊 Статистика")) {
                        std::unordered_map<std::string, int> categoryCount;
                        int olympicCount = 0;
                        for (const auto& rec : ui.records) {
                            std::string cat = rec.value("category", "Другое");
                            categoryCount[cat]++;
                            if (rec.value("olympic_status", false)) olympicCount++;
                        }

                        ImGui::Text("📈 Общая статистика:");
                        ImGui::Text("  • Всего записей: %d", ui.total);
                        ImGui::Text("  • Отображается: %zu", ui.records.size());
                        ImGui::Text("  • Олимпийских видов: %d", olympicCount);

                        ImGui::Separator();
                        ImGui::Text("📂 По категориям:");
                        for (const auto& [cat, count] : categoryCount) {
                            ImGui::Text("  • %s: %d", cat.c_str(), count);
                        }

                        ImGui::EndTabItem();
                    }
                }

                if (ui.showSearch) {
                    if (ImGui::BeginTabItem("🔍 Результат поиска")) {
                        if (ui.hasBinarySearchRecord) {
                            drawRecordCard(ui.binarySearchRecord, false);
                        } else {
                            ImGui::TextDisabled("Сначала выполните бинарный поиск.");
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