// Thread.cpp : 이 파일에는 'main' 함수가 포함됩니다. 거기서 프로그램 실행이 시작되고 종료됩니다.
//

#include <iostream>
#include <thread>
#include <vector>
#include <cassert>
#include <mutex>
#include <Windows.h>

int s;
int g_counter = 0;
int g_input = 0;
std::mutex g_mutex;
bool g_Run = false;

struct TagThreadData
{
    std::atomic_bool created;
    std::atomic_bool work;
    std::atomic_bool done;

	std::thread::id id;

	std::mutex mutex;
    std::thread::native_handle_type handle;
};
class CBase abstract
{
public:
    unsigned int Release()
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        if(0 == m_iRecCount)
        {
            Free();
            delete this;
            return 0;
        }
        return m_iRecCount--;
    }
    unsigned int AddRect()
    {
		std::lock_guard<std::mutex> guard(m_mutex);
        return ++m_iRecCount;
    }
protected:
    CBase()  = default;
    virtual ~CBase() = default;
private:
	std::mutex m_mutex;
    virtual void Free() = 0;

private:
    unsigned int m_iRecCount = 0;
};
class CGameObject : public CBase
{
public:
    CGameObject() : CBase()
    {
	    
    }
    virtual ~CGameObject()
    {
    }
    void Render()
    {
        using namespace std;
        cout << m_uiRenderNums << endl;
    }
    void Update()
    {
        ++m_uiUpdateNums;
    }
    void FixedUpdate()
    {
        m_uiRenderNums = m_uiRenderNums + m_uiUpdateNums;
        m_uiUpdateNums = 0;
    }
private:
    virtual void Free() override
    {
    }
private:
    std::mutex m_mutex;
    unsigned int m_uiUpdateNums = 0;
    unsigned int m_uiRenderNums = 0;
};

class CPhysicsThread
{
public:
    CPhysicsThread()
    {
        assert(sData.created == false&& L"Already Create Physics");
        sData.created = true;
    }
    ~CPhysicsThread()
    {

        using namespace std;
        cout << "~CPhysicsThread" << endl;
        sData.created = false;
	}

public:
    void Write(CGameObject* pObject)
    {
        std::lock_guard<std::mutex> guard(sData.mutex);
        m_vecReserveGameObjects.push_back(pObject);
        pObject->AddRect();
    }

    std::thread Run()
    {
        std::thread t(&CPhysicsThread::Progress, this);
        sData.handle = t.native_handle();
        sData.id = t.get_id();
        SetThreadDescription(sData.handle, L"Physics Thread");
        return t;
	}


private:
    void Initialize()
    {

    }

    void Progress()
    {
        Initialize();
        while (g_Run)
        {
            {
                std::lock_guard<std::mutex> guard(sData.mutex);
                m_vecReserveGameObjects.swap(m_vecCurrentGameObjects);
            }

            if(false == m_vecCurrentGameObjects.empty())
            {
                for (CGameObject* pObj : m_vecCurrentGameObjects)
                {
                    pObj->FixedUpdate();
                    pObj->Release();
                }
                m_vecCurrentGameObjects.clear();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));

        }
    }
private:
    static TagThreadData sData;
    std::vector<CGameObject*> m_vecReserveGameObjects = {};
    std::vector<CGameObject*> m_vecCurrentGameObjects = {};
};
TagThreadData CPhysicsThread::sData = {};

class CRenderThread
{
public:
    CRenderThread()
    {
        assert(sData.created == false && L"Already Create Render");
        sData.created = true;
    }
    ~CRenderThread()
    {
		using namespace std;
		cout << "~CRenderThread" << endl;  
        sData.created = false;
    }

public:
    std::thread Run()
    {
        std::thread t(&CRenderThread::Progress, this);
        sData.handle = t.native_handle();
        sData.id = t.get_id();
		SetThreadDescription(sData.handle, L"Render Thread");
        
        return t;
	}
    void Write(CGameObject* pObj)
    {
		std::lock_guard<std::mutex> guard(sData.mutex);
        m_vecEnterNums.push_back(pObj);
        pObj->AddRect();
    }
private:
    void Initialize()
    {
    }

    void Progress()
	{
        while (g_Run) {

            { 
                std::lock_guard<std::mutex> guard(sData.mutex);
                if (!m_vecEnterNums.empty())
                {
                    m_vecDrawNums.swap(m_vecEnterNums);
                }
            } 

            if (!m_vecDrawNums.empty())
            {
                system("cls");
                for (CGameObject* pObj : m_vecDrawNums)
                {
                    pObj->Render();
                    pObj->Release();
                }
                m_vecDrawNums.clear();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    }

private:
    static TagThreadData sData;
    std::vector<CGameObject*> m_vecEnterNums = {};
    std::vector<CGameObject*> m_vecDrawNums = {};
};
TagThreadData CRenderThread::sData = {};

CGameObject* pObj = nullptr;

int main()
{

    char ch = 0;

    //std::cout << sizeof(std::atomic_bool) << std::endl;

    //std::cin >> ch;
    //return 0 ;
	CPhysicsThread* physicsThread = new CPhysicsThread();
    CRenderThread* renderThread = new CRenderThread();

	g_Run = true;

    std::thread t1 = physicsThread->Run();
    std::thread t2 = renderThread->Run();
    int a = 0;
    CGameObject* pObj = new CGameObject();
    while(g_Run)
    {
        pObj->Update();
        physicsThread->Write(pObj);
        renderThread->Write(pObj);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

		using namespace std;
        cin >> a; 
        if(a != 0)
        {
            g_Run = false;
        }
    }
    
    t1.join();
    t2.join();

	delete pObj;
    pObj = nullptr;



    delete physicsThread;
    physicsThread = nullptr;
    delete renderThread;
	renderThread = nullptr;
    std::cout << "~Main" << std::endl;
    return 0;
}

