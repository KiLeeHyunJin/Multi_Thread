#include <iostream>
#include <thread>
#include <vector>
#include <cstdint>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>
#include <optional>
#include <variant>
#include <algorithm>
#include <deque>
#include <chrono>
#include <Windows.h>

// 병렬 파이프라인 버전
// 설계 원칙:
// 1) 더블 버퍼를 사용해 Physics는 항상 Back 버퍼(1 - front)를 쓰고,
//    Render는 Front 버퍼(front)를 읽는다. 이렇게 하면 두 스레드가 같은 버퍼를 동시에
//    쓰거나 읽는 일이 발생하지 않는다.
// 2) 버퍼 인덱스 교체는 atomic store/load (release/acquire)로 일관성을 보장.
// 3) EventQueue는 lock+deque로 안전하게 이벤트를 전달.
// 4) Main 스레드는 이벤트 처리와 상태 출력(또는 게임 로직)을 담당.

using Entity = uint32_t;
const Entity MAX_ENTITIES = 1000;

struct TransformComponent { double x = 0.0, y = 0.0; };
struct PhysicsComponent { double vx = 0.0, vy = 0.0; };
struct RenderComponent { char symbol = '\0'; };
struct HealthComponent { int health = 100; };

struct CollisionEvent { Entity a; Entity b; };
using GameEvent = std::variant<CollisionEvent>;

class EventQueue {
public:
    void Push(GameEvent event) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(std::move(event));
        m_cv.notify_one();
    }

    // 즉시 반환하는 팝 (std::nullopt 가능)
    std::optional<GameEvent> TryPop() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) return std::nullopt;
        GameEvent ev = std::move(m_queue.front());
        m_queue.pop_front();
        return ev;
    }

    // 이벤트가 올 때까지 블로킹 팝 (종료 플래그를 외부에서 검사해야 함)
    GameEvent WaitPop() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [&] { return !m_queue.empty(); });
        GameEvent ev = std::move(m_queue.front());
        m_queue.pop_front();
        return ev;
    }

    bool Empty() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.empty();
    }

private:
    std::deque<GameEvent> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
};

class Scene {
public:
    Scene() {
        m_transforms[0].resize(MAX_ENTITIES);
        m_transforms[1].resize(MAX_ENTITIES);
        m_physics.resize(MAX_ENTITIES);
        m_renders.resize(MAX_ENTITIES);
        m_healths.resize(MAX_ENTITIES);
        m_entity_active.resize(MAX_ENTITIES, false);
    }

    Entity CreateEntity() {
        for (Entity i = 0; i < MAX_ENTITIES; ++i) {
            if (!m_entity_active[i]) {
                m_entity_active[i] = true;
                return i;
            }
        }
        return MAX_ENTITIES;
    }

    // 기존 접근자 (편의성 유지)
    std::vector<TransformComponent>& GetTransforms_Front() { return m_transforms[m_frontBufferIndex.load()]; }
    std::vector<TransformComponent>& GetTransforms_Back() { return m_transforms[1 - m_frontBufferIndex.load()]; }

    void SwapTransformBuffers() { m_frontBufferIndex.store(1 - m_frontBufferIndex.load()); }

    std::vector<PhysicsComponent>& GetPhysics() { return m_physics; }
    std::vector<RenderComponent>& GetRenders() { return m_renders; }
    std::vector<HealthComponent>& GetHealths() { return m_healths; }
    const std::vector<bool>& GetActiveEntities() const { return m_entity_active; }

    // 병렬 파이프라인용 안전 접근자들:
    // front 인덱스 읽기/쓰기 (원자적, 메모리 순서 지정)
    int LoadFrontIndex() const { return m_frontBufferIndex.load(std::memory_order_acquire); }
    void StoreFrontIndex(int idx) { m_frontBufferIndex.store(idx, std::memory_order_release); }

    // 특정 인덱스의 transform 벡터 직접 참조 (주의: 호출자는 해당 버퍼를 다른 스레드가 쓰지 않음을 보장해야 함)
    std::vector<TransformComponent>& GetTransformsAt(int idx) { return m_transforms[idx]; }
    const std::vector<TransformComponent>& GetTransformsAtConst(int idx) const { return m_transforms[idx]; }

private:
    std::atomic<int> m_frontBufferIndex{ 0 };
    std::vector<TransformComponent> m_transforms[2]; // 더블 버퍼

    std::vector<PhysicsComponent> m_physics;
    std::vector<RenderComponent> m_renders;
    std::vector<HealthComponent> m_healths;
    std::vector<bool> m_entity_active;
};
std::vector<TransformComponent> m_transforms[2];
std::vector<PhysicsComponent> m_physics;
std::vector<RenderComponent> m_renders;
std::vector<HealthComponent> m_healths;
std::vector<bool> m_entity_active;
};

class PhysicsSystem {
public:
    // 기존 직렬 Update를 남겨둘 수 있지만 병렬 파이프라인에선 아래 UpdateParallel을 사용
    void Update(Scene& scene, EventQueue& events) {
        // legacy (unused)
        auto& transforms_front = scene.GetTransforms_Front();
        auto& transforms_back = scene.GetTransforms_Back();
        auto& physics = scene.GetPhysics();
        const auto& active = scene.GetActiveEntities();

        for (Entity i = 0; i < MAX_ENTITIES; ++i) {
            if (!active[i]) continue;
            transforms_back[i] = transforms_front[i];
            if (physics[i].vx != 0.0 || physics[i].vy != 0.0) {
                transforms_back[i].x += physics[i].vx;
                transforms_back[i].y += physics[i].vy;
            }
        }
        scene.SwapTransformBuffers();
    }

    // 병렬 파이프라인용 Update: frontIndex를 읽어 back 버퍼에 쓰고, 완료 시 atomic으로 front를 교체
    void UpdateParallel(Scene& scene, EventQueue& events) {
        // 현재 front 인덱스(렌더가 읽는 버퍼)
        int curFront = scene.LoadFrontIndex();
        int back = 1 - curFront;

        auto& transforms_front = scene.GetTransformsAt(curFront);
        auto& transforms_back = scene.GetTransformsAt(back);
        auto& physics = scene.GetPhysics();
        const auto& active = scene.GetActiveEntities();

        for (Entity i = 0; i < MAX_ENTITIES; ++i) {
            if (!active[i]) continue;

            // front의 값을 읽어 back으로 복사
            transforms_back[i] = transforms_front[i];

            if (physics[i].vx != 0.0 || physics[i].vy != 0.0) {
                transforms_back[i].x += physics[i].vx;
                transforms_back[i].y += physics[i].vy;

                // 경계 보정 및 이벤트
                if (transforms_back[i].x < 0) { transforms_back[i].x = 0; physics[i].vx *= -1; events.Push(CollisionEvent{ i, MAX_ENTITIES }); }
                if (transforms_back[i].x > 79) { transforms_back[i].x = 79; physics[i].vx *= -1; events.Push(CollisionEvent{ i, MAX_ENTITIES }); }
                if (transforms_back[i].y < 0) { transforms_back[i].y = 0; physics[i].vy *= -1; events.Push(CollisionEvent{ i, MAX_ENTITIES }); }
                if (transforms_back[i].y > 24) { transforms_back[i].y = 24; physics[i].vy *= -1; events.Push(CollisionEvent{ i, MAX_ENTITIES }); }
            }
        }

        // 모든 쓰기가 끝나면 front를 back으로 교체 (release)
        scene.StoreFrontIndex(back);
    }
};

struct RenderPacket { char symbol; int x, y; };

class RenderSystem {
public:
    void Collect(const Scene& scene, std::vector<RenderPacket>& packets) {
        packets.clear();
        const auto& transforms = scene.GetTransforms_Front();
        const auto& renders = scene.GetRenders();
        const auto& active = scene.GetActiveEntities();

        for (Entity i = 0; i < MAX_ENTITIES; ++i) {
            if (active[i] && renders[i].symbol != ' ') {
                packets.push_back({ renders[i].symbol, (int)transforms[i].x, (int)transforms[i].y });
            }
        }
    }

    // 병렬 파이프라인용 수집: 현재 front 인덱스를 atomic으로 읽고 해당 버퍼를 스냅샷처럼 사용
    void CollectParallel(const Scene& scene, std::vector<RenderPacket>& packets) {
        packets.clear();
        int curFront = scene.LoadFrontIndex();
        const auto& transforms = scene.GetTransformsAtConst(curFront);
        const auto& renders = scene.GetRenders();
        const auto& active = scene.GetActiveEntities();

        for (Entity i = 0; i < MAX_ENTITIES; ++i) {
            if (active[i] && renders[i].symbol != ' ') {
                packets.push_back({ renders[i].symbol, (int)transforms[i].x, (int)transforms[i].y });
            }
        }
    }
};

class RenderSystem {
public:
    void Collect(const Scene& scene, std::vector<RenderPacket>& packets) {
        packets.clear();
        const auto& transforms = scene.GetTransforms_Front();
        const auto& renders = scene.GetRenders();
        const auto& active = scene.GetActiveEntities();

        for (Entity i = 0; i < MAX_ENTITIES; ++i) {
            if (active[i] && renders[i].symbol != '\0') {
                packets.push_back({ renders[i].symbol, (int)transforms[i].x, (int)transforms[i].y });
            }
        }
    }
};

class DamageSystem {
public:
    // 이벤트를 모두 비울 때까지 처리한다 (메인 루프에서 호출)
    void DrainAndApply(Scene& scene, EventQueue& events) {
        auto& healths = scene.GetHealths();
        while (true) {
            auto evOpt = events.TryPop();
            if (!evOpt) break;
            std::visit([&](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, CollisionEvent>) {
                    if (arg.b == MAX_ENTITIES) {
                        if (healths[arg.a].health > 0) {
                            healths[arg.a].health = std::max(0, healths[arg.a].health - 10);
                            std::cout << "[Event] Entity " << arg.a << " hit a wall! HP: " << healths[arg.a].health << std::endl;
                        }
                    }
                }
                }, *evOpt);
        }
    }
};

void ClearScreen() {
    HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hStdOut == INVALID_HANDLE_VALUE) return;
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(hStdOut, &csbi)) return;
    DWORD cellCount = csbi.dwSize.X * csbi.dwSize.Y;
    DWORD written;
    COORD homeCoords = { 0, 0 };
    FillConsoleOutputCharacter(hStdOut, (TCHAR)' ', cellCount, homeCoords, &written);
    FillConsoleOutputAttribute(hStdOut, csbi.wAttributes, cellCount, homeCoords, &written);
    SetConsoleCursorPosition(hStdOut, homeCoords);
}

class Renderer {
public:
    void Draw(const std::vector<RenderPacket>& packets, const Scene& scene) {
        ClearScreen();
        char screen[25][81];
        for (int y = 0; y < 25; ++y) for (int x = 0; x < 81; ++x) screen[y][x] = ' ';
        for (int i = 0; i < 25; ++i) screen[i][80] = '\0';

        for (const auto& p : packets) {
            if (p.y >= 0 && p.y < 25 && p.x >= 0 && p.x < 80) screen[p.y][p.x] = p.symbol;
        }

        for (int y = 0; y < 25; ++y) printf("%s\n", screen[y]);
        printf("--------------------------------------------------------------------------------\n");
        const auto& active = scene.GetActiveEntities();
        const auto& healths = scene.GetHealths();
        for (Entity i = 0; i < 10; ++i) {
            if (active[i]) printf("[Entity %d] HP: %d | ", i, healths[i].health);
        }
        printf("\n");
    }
};

// 전역 동기화 상태 (메인, 물리, 렌더 간의 요청/완료 신호)
std::mutex g_mutex;
std::condition_variable g_cv;
bool g_requestPhysics = false;
bool g_physicsDone = false;
bool g_requestRender = false;
bool g_renderDone = false;
std::atomic<bool> g_running{ false };

class CPhysicsThread {
public:
    void Run(Scene& scene, EventQueue& events, PhysicsSystem& system) {
        while (g_running.load()) {
            // 요청을 기다림
            {
                std::unique_lock<std::mutex> lock(g_mutex);
                g_cv.wait(lock, [] { return !g_running.load() || g_requestPhysics; });
                if (!g_running.load()) break;
                g_requestPhysics = false; // consume request
            }

            // 실제 물리 업데이트
            system.Update(scene, events);

            // 버퍼 스왑
            scene.SwapTransformBuffers();

            // 완료 신호
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_physicsDone = true;
            }
            g_cv.notify_all();
        }
    }
};

class CRenderThread {
public:
    void Run(Scene& scene, RenderSystem& system, Renderer& renderer) {
        std::vector<RenderPacket> packets;
        while (g_running.load()) {
            // 렌더 요청 대기
            {
                std::unique_lock<std::mutex> lock(g_mutex);
                g_cv.wait(lock, [] { return !g_running.load() || g_requestRender; });
                if (!g_running.load()) break;
                g_requestRender = false; // consume request
            }

            // 수집 및 그리기
            system.Collect(scene, packets);
            renderer.Draw(packets, scene);

            // 완료 신호
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_renderDone = true;
            }
            g_cv.notify_all();
        }
    }
};

int main() {
    std::ios_base::sync_with_stdio(false);

    Scene scene;
    EventQueue events;
    PhysicsSystem physicsSystem;
    RenderSystem renderSystem;
    DamageSystem damageSystem;
    Renderer renderer;

    // 엔티티 생성
    Entity player = scene.CreateEntity();
    scene.GetTransforms_Front()[player] = { 40.0, 12.0 };
    scene.GetPhysics()[player] = { 0.5, 0.2 };
    scene.GetRenders()[player] = { '@' };
    scene.GetHealths()[player] = { 100 };

    Entity mob = scene.CreateEntity();
    scene.GetTransforms_Front()[mob] = { 10.0, 5.0 };
    scene.GetPhysics()[mob] = { -0.3, 0.1 };
    scene.GetRenders()[mob] = { 'M' };
    scene.GetHealths()[mob] = { 50 };

    // 런 스레드 시작 — 병렬 파이프라인 모드
    std::atomic<bool> running{ true };

    // Physics thread: 고정 타임스텝으로 백버퍼에 바로 쓰고, 완료되면 front 교체(atomic)
    std::thread tPhysics([&]() {
        const auto physicsDt = std::chrono::milliseconds(16); // 60Hz
        while (running.load()) {
            auto t0 = std::chrono::steady_clock::now();
            physicsSystem.UpdateParallel(scene, events);
            auto t1 = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
            if (elapsed < physicsDt) std::this_thread::sleep_for(physicsDt - elapsed);
        }
        });

    // Render thread: 가능한 빠르게 front 버퍼를 읽어 그림 (adaptive)
    std::thread tRender([&]() {
        std::vector<RenderPacket> packets;
        while (running.load()) {
            renderSystem.CollectParallel(scene, packets);
            renderer.Draw(packets, scene);
            std::this_thread::sleep_for(std::chrono::milliseconds(16)); // 간단한 VSync 유사 대기
        }
        });

    // Main: 이벤트 처리 및 상태 출력
    const int runSeconds = 10;
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count() < runSeconds) {
        // 이벤트 비동기 처리
        damageSystem.DrainAndApply(scene, events);
        // 주기적으로 상태 출력(들여다보기용)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // 종료
    running.store(false);
    tPhysics.join();
    tRender.join();

    printf("Execution finished.
        ");
        return 0;
}
