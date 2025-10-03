// Thread.cpp : 이 파일에는 'main' 함수가 포함됩니다. 거기서 프로그램 실행이 시작되고 종료됩니다.
//

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

#include <Windows.h>

// =================================================================
// 1. 컴포넌트 기반 아키텍처 (ECS) 및 기본 요소
// =================================================================

// 엔티티는 고유 ID입니다.
using Entity = uint32_t;
const Entity MAX_ENTITIES = 1000;

// --- 컴포넌트 정의 (데이터만 포함) ---
struct TransformComponent
{
    double x = 0.0, y = 0.0;
};

struct PhysicsComponent
{
    double vx = 0.0, vy = 0.0;
};

struct RenderComponent
{
    char symbol = '\0'; // 초기값은 렌더링되지 않도록 null 문자로 설정
};

struct HealthComponent
{
    int health = 100;
};

// --- 이벤트 큐 정의 (스레드 간 통신용) ---
struct CollisionEvent
{
    Entity a;
    Entity b; // b가 MAX_ENTITIES일 경우 벽과의 충돌을 의미
};

// 다른 이벤트들도 이곳에 추가 가능
using GameEvent = std::variant<CollisionEvent>;

class EventQueue
{
public:
    void Push(GameEvent event)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(event);
    }

    std::optional<GameEvent> Pop()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty())
        {
            return std::nullopt;
        }
        GameEvent event = m_queue.front();
        m_queue.erase(m_queue.begin());
        return event;
    }

private:
    std::mutex m_mutex;
    std::vector<GameEvent> m_queue;
};

// =================================================================
// 2. 씬(Scene) 클래스: ECS 데이터 관리자
// =================================================================
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

    // 더블 버퍼링을 위한 Transform 컴포넌트 접근자
    std::vector<TransformComponent>& GetTransforms_Front() { return m_transforms[m_frontBufferIndex]; }
    std::vector<TransformComponent>& GetTransforms_Back() { return m_transforms[1 - m_frontBufferIndex]; }

    void SwapTransformBuffers() {
        m_frontBufferIndex = 1 - m_frontBufferIndex;
    }

    std::vector<PhysicsComponent>& GetPhysics() { return m_physics; }
    std::vector<RenderComponent>& GetRenders() { return m_renders; }
    std::vector<HealthComponent>& GetHealths() { return m_healths; }
    const std::vector<bool>& GetActiveEntities() const { return m_entity_active; }

private:
    std::atomic<int> m_frontBufferIndex = 0;
    std::vector<TransformComponent> m_transforms[2]; // 더블 버퍼

    std::vector<PhysicsComponent> m_physics;
    std::vector<RenderComponent> m_renders;
    std::vector<HealthComponent> m_healths;
    std::vector<bool> m_entity_active;
};

// =================================================================
// 3. 시스템(System) 정의: 로직 담당
// =================================================================

class PhysicsSystem {
public:
    void Update(Scene& scene, EventQueue& events) {
        auto& transforms_front = scene.GetTransforms_Front();
        auto& transforms_back = scene.GetTransforms_Back();
        auto& physics = scene.GetPhysics();
        const auto& active = scene.GetActiveEntities();

        for (Entity i = 0; i < MAX_ENTITIES; ++i) {
            if (active[i]) {
                // 물리 컴포넌트가 있는 객체만 처리
                if (physics[i].vx != 0.0 || physics[i].vy != 0.0) {
                    transforms_back[i] = transforms_front[i];
                    transforms_back[i].x += physics[i].vx;
                    transforms_back[i].y += physics[i].vy;

                    if (transforms_back[i].x < 0 || transforms_back[i].x > 79) {
                        physics[i].vx *= -1;
                        events.Push(CollisionEvent{ i, MAX_ENTITIES });
                    }
                    if (transforms_back[i].y < 0 || transforms_back[i].y > 24) {
                        physics[i].vy *= -1;
                        events.Push(CollisionEvent{ i, MAX_ENTITIES });
                    }
                }
                else {
                    // 물리 컴포넌트가 없어도 back 버퍼 상태는 동기화
                    transforms_back[i] = transforms_front[i];
                }
            }
        }
    }
};

struct RenderPacket {
    char symbol;
    int x, y;
};

class RenderSystem {
public:
    void Collect(Scene& scene, std::vector<RenderPacket>& packets) {
        packets.clear();
        auto& transforms = scene.GetTransforms_Front();
        auto& renders = scene.GetRenders();
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
    void Update(Scene& scene, EventQueue& events)
    {
        while (auto eventOpt = events.Pop())
        {
            std::visit([&](auto&& arg)
                {
                    using T = std::decay_t<decltype(arg)>;
                    if constexpr (std::is_same_v<T, CollisionEvent>)
                    {
                        if (arg.b == MAX_ENTITIES)
                        { // 벽 충돌
                            auto& healths = scene.GetHealths();
                            if (healths[arg.a].health > 0)
                            {
                                healths[arg.a].health = std::max(0, healths[arg.a].health - 10);
                                std::cout << "[Event] Entity " << arg.a << " hit a wall! HP: " << healths[arg.a].health << std::endl;
                            }
                        }
                    }
                }, *eventOpt);
        }
    }
};

// =================================================================
// 4. 렌더러 및 스레드 클래스 수정
// =================================================================

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
        char screen[25][81] = {}; // Null-terminated strings
        memset(screen, ' ', sizeof(screen));
        for (int i = 0; i < 25; ++i) screen[i][80] = '\0';

        for (const auto& packet : packets) {
            if (packet.y >= 0 && packet.y < 25 && packet.x >= 0 && packet.x < 80) {
                screen[packet.y][packet.x] = packet.symbol;
            }
        }

        for (int y = 0; y < 25; ++y) {
            printf("%s\n", screen[y]);
        }
        // UI 정보 출력
        printf("--------------------------------------------------------------------------------\n");
        const auto& active = scene.GetActiveEntities();
        const auto& healths = scene.GetHealths();
        for (Entity i = 0; i < 10; ++i) { // 처음 10개 엔티티 정보만 표시
            if (active[i]) {
                printf("[Entity %d] HP: %d | ", i, healths[i].health);
            }
        }
        printf("\n");

    }
};

std::atomic<bool> g_Run = false;
std::condition_variable g_cv;
std::mutex g_mutex;
bool g_physicsWork = false;
bool g_renderWork = false;

class CPhysicsThread {
public:
    void Run(Scene& scene, EventQueue& events, PhysicsSystem& system) {
        while (g_Run) {
            {
                std::unique_lock<std::mutex> lock(g_mutex);
                g_cv.wait(lock, [] { return !g_Run || g_physicsWork; });
                if (!g_Run) break;
            }

            system.Update(scene, events);
            scene.SwapTransformBuffers();

            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_physicsWork = false;
                g_renderWork = true;
            }
            g_cv.notify_all();
        }
    }
};

class CRenderThread {
public:
    void Run(Scene& scene, RenderSystem& system, Renderer& renderer) {
        std::vector<RenderPacket> packets;
        while (g_Run) {
            {
                std::unique_lock<std::mutex> lock(g_mutex);
                g_cv.wait(lock, [] { return !g_Run || g_renderWork; });
                if (!g_Run) break;
            }

            system.Collect(scene, packets);
            renderer.Draw(packets, scene);

            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_renderWork = false;
            }
        }
    }
};

// =================================================================
// 5. Main 함수: 모든 것을 연결하고 실행
// =================================================================

int main() {
    std::ios_base::sync_with_stdio(false);

    Scene scene;
    EventQueue events;
    PhysicsSystem physicsSystem;
    RenderSystem renderSystem;
    DamageSystem damageSystem;
    Renderer renderer;
    CPhysicsThread physicsThread;
    CRenderThread renderThread;

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

    g_Run = true;
    std::thread t1(&CPhysicsThread::Run, &physicsThread, std::ref(scene), std::ref(events), std::ref(physicsSystem));
    std::thread t2(&CRenderThread::Run, &renderThread, std::ref(scene), std::ref(renderSystem), std::ref(renderer));

    for (int i = 0; i < 500; ++i) {
        damageSystem.Update(scene, events);

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_physicsWork = true;
        }
        g_cv.notify_all();

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    g_Run = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_physicsWork = true;
        g_renderWork = true;
    }
    g_cv.notify_all();
    t1.join();
    t2.join();

    printf("Execution finished.\n");
    return 0;
}
