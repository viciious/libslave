#include <boost/test/included/unit_test.hpp>
using namespace boost::unit_test;

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/mpl/int.hpp>
#include <boost/mpl/list.hpp>
#include <boost/thread.hpp>
#include <fstream>
#include <cfloat>
#include <mutex>
#include <condition_variable>
#include "Slave.h"
#include "nanomysql.h"
#include "types.h"

namespace // anonymous
{
    const std::string TestDataDir = "test/data/";

    struct config
    {
        std::string mysql_host;
        int         mysql_port;
        std::string mysql_db;
        std::string mysql_user;
        std::string mysql_pass;

        config()
        :   mysql_host("localhost")
        ,   mysql_port(3306)
        ,   mysql_db("test")
        ,   mysql_user("root")
        {}

        void load(const std::string& fn)
        {
            std::ifstream f(fn.c_str());
            if (!f)
                throw std::runtime_error("can't open config file '" + fn + "'");

            std::string line;
            while (getline(f, line))
            {
                if (line.empty())
                    continue;
                std::vector<std::string> tokens;
                boost::algorithm::split(tokens, line, boost::algorithm::is_any_of(" ="), boost::algorithm::token_compress_on);
                if (tokens.empty())
                    continue;
                if (tokens.size() != 2)
                    throw std::runtime_error("Malformed string '" + line + "' in the config file '" + fn + "'");

                if (tokens.front() == "mysql_host")
                    mysql_host = tokens.back();
                else if (tokens.front() == "mysql_port")
                    mysql_port = atoi(tokens.back().c_str());
                else if (tokens.front() == "mysql_db")
                    mysql_db = tokens.back();
                else if (tokens.front() == "mysql_user")
                    mysql_user = tokens.back();
                else if (tokens.front() == "mysql_pass")
                    mysql_pass = tokens.back();
                else
                    throw std::runtime_error("unknown option '" + tokens.front() + "' in config file '" + fn + "'");
            }
        }
    };

    template <typename T>
    bool not_equal(const T& a, const T& b)
    {
        return a != b;
    }

    bool not_equal(double a, double b)
    {
        return fabs(a-b) > DBL_EPSILON * fmax(fabs(a),fabs(b));
    }

    template <typename T>
    class Atomic
    {
        volatile T m_Value;

    public:
        typedef T value_type;

        Atomic() {}
        Atomic(T aValue) : m_Value(aValue) {}
        Atomic(const Atomic& r) : m_Value(r) {} // использует operator T
        Atomic& operator= (const Atomic& r) { return operator = (static_cast<T>(r)); }

        T operator++ ()         { return __sync_add_and_fetch(&m_Value, 1); }
        T operator++ (int)      { return __sync_fetch_and_add(&m_Value, 1); }
        T operator+= (T aValue) { return __sync_add_and_fetch(&m_Value, aValue); }

        T operator-- ()         { return __sync_sub_and_fetch(&m_Value, 1); }
        T operator-- (int)      { return __sync_fetch_and_sub(&m_Value, 1); }
        T operator-= (T aValue) { return __sync_sub_and_fetch(&m_Value, aValue); }

        Atomic& operator= (T aValue) { __sync_lock_test_and_set(&m_Value, aValue); return *this; }

        operator T() const
        {
            __sync_synchronize();
            return m_Value;
        }
    };

    struct Fixture
    {
        struct TestExtState : public slave::ExtStateIface
        {
            std::mutex m_Mutex;
            std::condition_variable m_CondVariable;

            TestExtState() : master_log_pos(0), intransaction_pos(0) {}

            virtual slave::State getState() { return slave::State(); }
            virtual void setConnecting() {}
            virtual time_t getConnectTime() { return 0; }
            virtual void setLastFilteredUpdateTime() {}
            virtual time_t getLastFilteredUpdateTime() { return 0; }
            virtual void setLastEventTimePos(time_t t, unsigned long pos) { intransaction_pos = pos; }
            virtual time_t getLastUpdateTime() { return 0; }
            virtual time_t getLastEventTime() { return 0; }
            virtual unsigned long getIntransactionPos() { return intransaction_pos; }
            virtual void setMasterLogNamePos(const std::string& log_name, unsigned long pos)
            {
                {
                    std::lock_guard<std::mutex> lock(m_Mutex);
                    master_log_name = log_name;
                    master_log_pos = intransaction_pos = pos;
                }
                m_CondVariable.notify_one();
            }
            virtual unsigned long getMasterLogPos() { return master_log_pos; }
            virtual std::string getMasterLogName() { return master_log_name; }
            virtual void saveMasterInfo() {}
            virtual bool loadMasterInfo(std::string& logname, unsigned long& pos) { logname.clear(); pos = 0; return false; }
            virtual unsigned int getConnectCount() { return 0; }
            virtual void setStateProcessing(bool _state) {}
            virtual bool getStateProcessing() { return false; }
            virtual void initTableCount(const std::string& t) {}
            virtual void incTableCount(const std::string& t) {}

        private:
            std::string     master_log_name;
            unsigned long   master_log_pos;
            unsigned long   intransaction_pos;
        };

        config cfg;
        TestExtState m_ExtState;
        slave::Slave m_Slave;
        boost::shared_ptr<nanomysql::Connection> conn;

        struct StopFlag
        {
            Atomic<int> m_StopFlag;
            Atomic<int> m_SlaveStarted;
            Atomic<int> m_SleepFlag;

            StopFlag()
            :   m_StopFlag(false)
            ,   m_SlaveStarted(false)
            ,   m_SleepFlag(false)
            {}

            bool operator() ()
            {
                m_SlaveStarted = true;
                if (m_SleepFlag)
                {
                    ::sleep(1);
                    m_SleepFlag = false;
                }
                return m_StopFlag;
            }
        };

        StopFlag        m_StopFlag;
        boost::thread   m_SlaveThread;

        struct Callback
        {
            boost::mutex m_Mutex;
            slave::callback m_Callback;
            Atomic<int> m_UnwantedCalls;

            Callback() : m_UnwantedCalls(0) {}

            void operator() (slave::RecordSet& rs)
            {
                boost::mutex::scoped_lock l(m_Mutex);
                if (!m_Callback.empty())
                    m_Callback(rs);
                else
                    ++m_UnwantedCalls;
            }

            void setCallback(slave::callback c)
            {
                boost::mutex::scoped_lock l(m_Mutex);
                m_Callback = c;
            }

            void setCallback()
            {
                boost::mutex::scoped_lock l(m_Mutex);
                m_Callback.clear();
            }
        };

        Callback m_Callback;
        slave::EventKind m_Filter;

        void startSlave()
        {
            m_StopFlag.m_StopFlag = false;

            m_Slave.createDatabaseStructure();

            // Запускаем libslave с нашим кастомной функцией остановки, которая еще и сигнализирует,
            // когда слейв прочитал позицию бинлога и готов получать сообщения
            m_SlaveThread = boost::thread([this] () { m_Slave.get_remote_binlog(std::ref(m_StopFlag)); });

            // Ждем, чтобы libslave запустился - не более 1000 раз по 1 мс
            const timespec ts = {0 , 1000000};
            size_t i = 0;
            for (; i < 1000; ++i)
            {
                ::nanosleep(&ts, NULL);
                if (m_StopFlag.m_SlaveStarted)
                    break;
            }
            if (1000 == i)
                BOOST_FAIL ("Can't connect to mysql via libslave in 1 second");
        }

        Fixture(slave::EventKind filter = slave::eAll) : m_Slave(m_ExtState), m_Filter(filter)
        {
            cfg.load(TestDataDir + "mysql.conf");

            conn.reset(new nanomysql::Connection(cfg.mysql_host, cfg.mysql_user, cfg.mysql_pass, cfg.mysql_db));
            conn->query("set names utf8");
            // Создаем таблицу, т.к. если ее нет, libslave ругнется на ее отсутствие, и тест закончится
            conn->query("CREATE TABLE IF NOT EXISTS test (tmp int)");

            slave::MasterInfo sMasterInfo;
            sMasterInfo.host = cfg.mysql_host;
            sMasterInfo.port = cfg.mysql_port;
            sMasterInfo.user = cfg.mysql_user;
            sMasterInfo.password = cfg.mysql_pass;

            m_Slave.setMasterInfo(sMasterInfo);
            // Ставим колбек из фиксчи - а он будет вызывать колбеки, которые ему будут ставить в тестах
            m_Slave.setCallback(cfg.mysql_db, "test", boost::ref(m_Callback), filter);
            m_Slave.init();
            startSlave();
        }

        void stopSlave()
        {
            m_StopFlag.m_StopFlag = true;
            m_Slave.close_connection();
            if (m_SlaveThread.joinable())
                m_SlaveThread.join();
        }

        ~Fixture()
        {
            stopSlave();
        }

        template<typename T>
        struct Collector
        {
            typedef slave::RecordSet::TypeEvent TypeEvent;
            typedef boost::optional<T> Row;
            typedef std::tuple<TypeEvent, Row, Row> Event;
            typedef std::vector<Event> EventVector;
            EventVector data;

            static Row extract(const slave::Row& row)
            {
                if (row.size() > 1)
                {
                    std::ostringstream str;
                    str << "Row size is " << row.size();
                    throw std::runtime_error(str.str());
                }
                const slave::Row::const_iterator it = row.find("value");
                if (row.end() != it)
                    return boost::any_cast<T>(it->second.second);
                else
                    return Row();
            }

            void operator()(const slave::RecordSet& rs)
            {
                data.emplace_back(std::make_tuple(rs.type_event, extract(rs.m_old_row), extract(rs.m_row)));
            }

            static void expectNothing(const Row& row, const std::string& name,
                                      const std::string& aErrorMessage)
            {
                if (row)
                    BOOST_ERROR("Has " << name << " image with '" << row.get()
                                << "' value, expected nothing during" << aErrorMessage);
            }

            static void expectValue(const T& value, const Row& row, const std::string& name,
                                    const std::string& aErrorMessage)
            {
                if (!row)
                    BOOST_ERROR("Has not " << name << " image, expected '" << value << "' during" << aErrorMessage);
                if (not_equal(row.get(), value))
                    BOOST_ERROR("Has invalid " << name << " image with '" << row.get() << "'"
                                << "while expected '"<< value << "' during " << aErrorMessage);
            }

            static void expectEventType(const TypeEvent& expected, const TypeEvent& value, const std::string& name,
                                        const std::string& aErrorMessage)
            {
                if (not_equal(expected, value))
                    BOOST_ERROR("Has invalid " << name << " image with '" << value << "'"
                                << "while expected '"<< expected << "' during " << aErrorMessage);
            }

            void checkInsert(const T& t, const std::string& aErrorMessage) const
            {
                if (data.size() != 1)
                    BOOST_ERROR ("Have invalid call count: " << data.size() << " for " << aErrorMessage);
                const auto& tuple = data.front();
                expectEventType(TypeEvent::Write, std::get<0>(tuple), "TYPE_EVENT", aErrorMessage);
                expectNothing(std::get<1>(tuple), "BEFORE", aErrorMessage);
                expectValue(t, std::get<2>(tuple), "AFTER", aErrorMessage);
            }

            void checkUpdate(const T& was, const T& now, const std::string& aErrorMessage) const
            {
                if (data.size() != 1)
                    BOOST_ERROR ("Have invalid call count: " << data.size() << " for " << aErrorMessage);
                const auto& tuple = data.front();
                expectEventType(TypeEvent::Update, std::get<0>(tuple), "TYPE_EVENT", aErrorMessage);
                expectValue(was, std::get<1>(tuple), "BEFORE", aErrorMessage);
                expectValue(now, std::get<2>(tuple), "AFTER", aErrorMessage);
            }

            void checkDelete(const T& was, const std::string& aErrorMessage) const
            {
                if (data.size() != 1)
                    BOOST_ERROR ("Have invalid call count: " << data.size() << " for " << aErrorMessage);
                const auto& tuple = data.front();
                expectEventType(TypeEvent::Delete, std::get<0>(tuple), "TYPE_EVENT", aErrorMessage);
                expectValue(was, std::get<2>(tuple), "BEFORE", aErrorMessage);
                expectNothing(std::get<1>(tuple), "AFTER", aErrorMessage);
            }

            void checkNothing(const std::string& aErrorMessage)
            {
                if (!data.empty())
                    BOOST_ERROR ("Have invalid call count: " << data.size() << " for " << aErrorMessage);
            }
        };

        template<typename T>
        bool waitCall(const Collector<T>& aCallback)
        {
            std::string   log_name;
            unsigned long log_pos;
            conn->query("SHOW MASTER STATUS");
            conn->use([&log_name, &log_pos](const nanomysql::fields_t& row)
            {
                log_name = row.at("File").data;
                log_pos = std::stoul(row.at("Position").data);
            });

            std::unique_lock<std::mutex> lock(m_ExtState.m_Mutex);
            if (!m_ExtState.m_CondVariable.wait_for(lock, std::chrono::milliseconds(2000), [this, &log_name, &log_pos]
            {
                return log_name == m_ExtState.getMasterLogName() && \
                       log_pos  == m_ExtState.getMasterLogPos();
            }))
                BOOST_ERROR("Condition variable timed out");

            if (aCallback.data.empty())
                return false;
            return true;
        }

        bool shouldProcess(slave::EventKind filter, slave::EventKind sort)
        {
            return (filter & sort) == sort;
        }

        template<typename T, typename F>
        void check(F f, const std::string& aQuery, const std::string& aErrorMsg, slave::EventKind sort)
        {
            // Устанавливаем в libslave колбек для проверки этого значения
            Collector<T> sCallback;
            m_Callback.setCallback(std::ref(sCallback));
            // Проверяем, что не было нежелательных вызовов до этого
            if (0 != m_Callback.m_UnwantedCalls)
                BOOST_ERROR("Unwanted calls before this case: " << m_Callback.m_UnwantedCalls << aErrorMsg);

            // Модифицируем таблицу
            conn->query(aQuery);

            if (waitCall(sCallback))
            {
                if (shouldProcess(m_Filter, sort))
                    f(sCallback);
                else
                    BOOST_ERROR("Have unfiltered calls to libslave callback");
            }
            else
            {
                if (shouldProcess(m_Filter, sort))
                    BOOST_ERROR("Have no calls to libslave callback");
            }

            // Убираем наш колбек, т.к. он при выходе из блока уничтожится, заодно чтобы
            // строку он не мучал больше, пока мы ее проверяем
            m_Callback.setCallback();
        }

        template<typename T>
        struct Line
        {
            std::string type;
            std::string filename;
            std::string line;
            size_t      lineNumber;
            std::string insert;
            T           expected;
        };

        template<typename T> static std::string errorMessage(const Line<T>& c)
        {
            return "(we are now on file '" + c.filename + "' line " + std::to_string(c.lineNumber) + ": '" + c.line + "')";
        }

        template <typename T>
        void checkInsertValue(T t, const std::string& aValue, const std::string& aErrorMessage)
        {
            check<T>([&t, &aErrorMessage](const Collector<T>& collector)
                     { collector.checkInsert(t, aErrorMessage); },
                     "INSERT INTO test VALUES (" + aValue + ")", aErrorMessage, slave::eInsert);
        }

        template<typename T> void checkInsert(const Line<T>& line)
        {
            checkInsertValue<T>(line.expected, line.insert, errorMessage(line));
        }

        template<typename T>
        void checkUpdate(Line<T> was, Line<T> now)
        {
            check<T>([&was, &now](const Collector<T>& collector)
                     { collector.checkUpdate(was.expected, now.expected, errorMessage(now)); },
                     "UPDATE test SET value=" + now.insert, errorMessage(now), slave::eUpdate);
        }

        template <typename T>
        void checkDeleteValue(T was, const std::string& aValue, const std::string& aErrorMessage)
        {
            check<T>([&was, &aErrorMessage](const Collector<T>& collector)
                     { collector.checkDelete(was, aErrorMessage); },
                     "DELETE FROM test", aErrorMessage, slave::eDelete);
        }

        template<typename T> void recreate(boost::shared_ptr<nanomysql::Connection>& conn,
                                           const Line<T>& c)
        {
            const std::string sDropTableQuery = "DROP TABLE IF EXISTS test";
            conn->query(sDropTableQuery);
            const std::string sCreateTableQuery = "CREATE TABLE test (value " + c.type + ") DEFAULT CHARSET=utf8";
            conn->query(sCreateTableQuery);
        }

        template<typename T> void testInsert(boost::shared_ptr<nanomysql::Connection>& conn,
                                             const std::vector<Line<T>>& data)
        {
            for (const Line<T>& c : data)
            {
                recreate(conn, c);
                checkInsertValue<T>(c.expected, c.insert, errorMessage(c));
            }
        }

        template<typename T> void testUpdate(boost::shared_ptr<nanomysql::Connection>& conn,
                                             const std::vector<Line<T>>& data)
        {
            for (std::size_t i = 0; i < data.size(); ++i)
                if (i == 0)
                {
                    recreate(conn, data[0]);
                    checkInsert<T>(data[0]);
                }
                else
                {
                    // стоит if, т.к. в противном случае callback не дернется
                    if (data[i-1].expected != data[i].expected)
                        checkUpdate<T>(data[i-1], data[i]);
                }
            if (data.back().expected != data.front().expected)
                checkUpdate<T>(data.back(), data.front());
        }

        template<typename T> void testDelete(boost::shared_ptr<nanomysql::Connection>& conn,
                                             const std::vector<Line<T>>& data)
        {
            for (const Line<T>& c : data)
            {
                recreate(conn, c);
                checkInsertValue<T>(c.expected, c.insert, errorMessage(c));

                checkDeleteValue<T>(c.expected, c.insert, errorMessage(c));
            }
        }

        template<typename T> void testAll(boost::shared_ptr<nanomysql::Connection>& conn,
                                          const std::vector<Line<T>>& data)
        {
            if (data.empty())
                return;
            testInsert(conn, data);
            testUpdate(conn, data);
            testDelete(conn, data);
        }
    };


    void test_HelloWorld()
    {
        std::cout << "You probably should specify parameters to mysql in the file " << TestDataDir << "mysql.conf first" << std::endl;
    }

    // Проверяем, что если останавливаем слейв, он в дальнейшем продолжит читать с той же позиции
    void test_StartStopPosition()
    {
        Fixture f;
        // Создаем нужную таблицу
        f.conn->query("DROP TABLE IF EXISTS test");
        f.conn->query("CREATE TABLE IF NOT EXISTS test (value int)");

        f.checkInsertValue(uint32_t(12321), "12321", "");

        f.stopSlave();

        f.conn->query("INSERT INTO test VALUES (345234)");

        Fixture::Collector<uint32_t> sCallback;
        f.m_Callback.setCallback(std::ref(sCallback));

        f.startSlave();

        auto sErrorMessage = "start/stop test";
        if (!f.waitCall(sCallback))
            BOOST_ERROR("Have no calls to libslave callback for " << sErrorMessage);
        sCallback.checkInsert(345234, sErrorMessage);

        // Убираем наш колбек, т.к. он при выходе из блока уничтожится, заодно чтобы
        // строку он не мучал больше, пока мы ее проверяем
        f.m_Callback.setCallback();

        // Проверяем, что не было нежелательных вызовов до этого
        if (0 != f.m_Callback.m_UnwantedCalls)
            BOOST_ERROR("Unwanted calls before this case: " << f.m_Callback.m_UnwantedCalls);
    }

    struct CheckBinlogPos
    {
        const slave::Slave& m_Slave;
        slave::Slave::binlog_pos_t m_LastPos;

        CheckBinlogPos(const slave::Slave& aSlave, const slave::Slave::binlog_pos_t& aLastPos)
        :   m_Slave(aSlave), m_LastPos(aLastPos)
        {}

        bool operator() () const
        {
            const slave::MasterInfo& sMasterInfo = m_Slave.masterInfo();
            if (sMasterInfo.master_log_name > m_LastPos.first
            || (sMasterInfo.master_log_name == m_LastPos.first
                && sMasterInfo.master_log_pos >= m_LastPos.second))
                return true;
            return false;
        }
    };

    struct CallbackCounter
    {
        Atomic<int> counter;
        std::string fail;

        CallbackCounter() : counter(0) {}

        void operator() (const slave::RecordSet& rs)
        {
            if (++counter > 2)
                fail = std::to_string(counter) + " calls on CallbackCounter";
        }
    };

    // Проверяем, работает ли ручное выставление позиции бинлога
    void test_SetBinlogPos()
    {
        Fixture f;
        // Создаем нужную таблицу
        f.conn->query("DROP TABLE IF EXISTS test");
        f.conn->query("CREATE TABLE IF NOT EXISTS test (value int)");

        f.checkInsertValue(uint32_t(12321), "12321", "");

        // Запоминаем позицию
        const slave::Slave::binlog_pos_t sInitialBinlogPos = f.m_Slave.getLastBinlog();

        // Вставляем значение, читаем его
        f.checkInsertValue(uint32_t(12322), "12322", "");

        f.stopSlave();

        // Вставляем новое значение
        f.conn->query("INSERT INTO test VALUES (345234)");

        // И получаем новую позицию
        const slave::Slave::binlog_pos_t sCurBinlogPos = f.m_Slave.getLastBinlog();
        BOOST_CHECK_NE(sCurBinlogPos.second, sInitialBinlogPos.second);

        // Теперь выставляем в слейв старую позицию и проверяем, что 2 INSERTа прочтутся (12322 и 345234)
        slave::MasterInfo sMasterInfo = f.m_Slave.masterInfo();
        sMasterInfo.master_log_name = sInitialBinlogPos.first;
        sMasterInfo.master_log_pos = sInitialBinlogPos.second;
        f.m_Slave.setMasterInfo(sMasterInfo);

        CallbackCounter sCallback;
        f.m_Callback.setCallback(std::ref(sCallback));
        if (0 != f.m_Callback.m_UnwantedCalls)
        {
            BOOST_ERROR("Unwanted calls before this case: " << f.m_Callback.m_UnwantedCalls);
        }

        f.m_SlaveThread = boost::thread([&f, sCurBinlogPos] () { f.m_Slave.get_remote_binlog(CheckBinlogPos(f.m_Slave, sCurBinlogPos)); });

        // Ждем отработки колбека максимум 1 секунду
        const timespec ts = {0 , 1000000};
        size_t i = 0;
        for (; i < 1000; ++i)
        {
            ::nanosleep(&ts, NULL);
            if (sCallback.counter >= 2)
                break;
        }
        if (sCallback.counter < 2)
            BOOST_ERROR ("Have less than two calls to libslave callback for 1 second");

        // Убираем наш колбек, т.к. он при выходе из блока уничтожится, заодно чтобы
        // строку он не мучал больше, пока мы ее проверяем
        f.m_Callback.setCallback();

        if (!sCallback.fail.empty())
            BOOST_ERROR(sCallback.fail);

        BOOST_CHECK_MESSAGE (f.m_SlaveThread.joinable(), "m_Slave.get_remote_binlog is not finished yet and will be never!");
    }

    // Проверяем, что если соединение с базой рвется (без выхода из get_remote_binlog), то начинаем читать оттуда, где остановились
    void test_Disconnect()
    {
        Fixture f;
        // Создаем нужную таблицу
        f.conn->query("DROP TABLE IF EXISTS test");
        f.conn->query("CREATE TABLE IF NOT EXISTS test (value int)");

        f.checkInsertValue(uint32_t(12321), "12321", "");

        f.m_StopFlag.m_SleepFlag = true;
        f.m_Slave.close_connection();

        f.conn->query("INSERT INTO test VALUES (345234)");

        Fixture::Collector<uint32_t> sCallback;
        f.m_Callback.setCallback(std::ref(sCallback));

        auto sErrorMessage = "disconnect test";
        if (!f.waitCall(sCallback))
            BOOST_ERROR("Have no calls to libslave callback for " << sErrorMessage);
        sCallback.checkInsert(345234, sErrorMessage);

        // Убираем наш колбек, т.к. он при выходе из блока уничтожится, заодно чтобы
        // строку он не мучал больше, пока мы ее проверяем
        f.m_Callback.setCallback();

        // Проверяем, что не было нежелательных вызовов до этого
        if (0 != f.m_Callback.m_UnwantedCalls)
            BOOST_ERROR("Unwanted calls before this case: " << f.m_Callback.m_UnwantedCalls);
    }

    enum MYSQL_TYPE
    {
        MYSQL_TINYINT,
        MYSQL_INT,
        MYSQL_BIGINT,
        MYSQL_CHAR,
        MYSQL_VARCHAR,
        MYSQL_TINYTEXT,
        MYSQL_TEXT,
        MYSQL_DECIMAL,
        MYSQL_BIT,
        MYSQL_SET
    };

    template <MYSQL_TYPE T>
    struct MYSQL_type_traits;

    template <>
    struct MYSQL_type_traits<MYSQL_INT>
    {
        typedef slave::types::MY_INT slave_type;
        static const std::string name;
    };
    const std::string MYSQL_type_traits<MYSQL_INT>::name = "INT";

    template <>
    struct MYSQL_type_traits<MYSQL_BIGINT>
    {
        typedef slave::types::MY_BIGINT slave_type;
        static const std::string name;
    };
    const std::string MYSQL_type_traits<MYSQL_BIGINT>::name = "BIGINT";

    template <>
    struct MYSQL_type_traits<MYSQL_CHAR>
    {
        typedef slave::types::MY_CHAR slave_type;
        static const std::string name;
    };
    const std::string MYSQL_type_traits<MYSQL_CHAR>::name = "CHAR";

    template <>
    struct MYSQL_type_traits<MYSQL_VARCHAR>
    {
        typedef slave::types::MY_VARCHAR slave_type;
        static const std::string name;
    };
    const std::string MYSQL_type_traits<MYSQL_VARCHAR>::name = "VARCHAR";

    template <>
    struct MYSQL_type_traits<MYSQL_TINYTEXT>
    {
        typedef slave::types::MY_TINYTEXT slave_type;
        static const std::string name;
    };
    const std::string MYSQL_type_traits<MYSQL_TINYTEXT>::name = "TINYTEXT";

    template <>
    struct MYSQL_type_traits<MYSQL_TEXT>
    {
        typedef slave::types::MY_TEXT slave_type;
        static const std::string name;
    };
    const std::string MYSQL_type_traits<MYSQL_TEXT>::name = "TEXT";

    template <>
    struct MYSQL_type_traits<MYSQL_DECIMAL>
    {
        typedef slave::types::MY_DECIMAL slave_type;
        static const std::string name;
    };
    const std::string MYSQL_type_traits<MYSQL_DECIMAL>::name = "DECIMAL";

    template <>
    struct MYSQL_type_traits<MYSQL_BIT>
    {
        typedef slave::types::MY_BIT slave_type;
        static const std::string name;
    };
    const std::string MYSQL_type_traits<MYSQL_BIT>::name = "BIT";

    template <>
    struct MYSQL_type_traits<MYSQL_SET>
    {
        typedef slave::types::MY_SET slave_type;
        static const std::string name;
    };
    const std::string MYSQL_type_traits<MYSQL_SET>::name = "SET";

    template <typename T>
    void getValue(const std::string& s, T& t)
    {
        std::istringstream is;
        is.str(s);
        is >> t;
    }

    void getValue(const std::string& s, std::string& t)
    {
        t = s;
        // Убираем ведущий пробел
        t.erase(0, 1);
    }

    template<typename T>
    void testOneType(Fixture& fixture)
    {
        typedef MYSQL_type_traits<MYSQL_TYPE(T::value)> type_traits;
        typedef typename type_traits::slave_type slave_type;

        const std::string sDataFilename = TestDataDir + "OneField/" + type_traits::name;
        std::ifstream f(sDataFilename.c_str());
        BOOST_REQUIRE_MESSAGE(f, "Cannot open file with data: '" << sDataFilename << "'");
        std::string line;
        size_t line_num = 0;
        std::vector<Fixture::Line<slave_type>> data;
        std::string type;
        while (getline(f, line))
        {
            ++line_num;
            if (line.empty())
                continue;
            std::vector<std::string> tokens;
            const char* sDelimiters = ",";
            if ("SET" == type_traits::name)
                sDelimiters=";";
            boost::algorithm::split(tokens, line, boost::algorithm::is_any_of(sDelimiters), boost::algorithm::token_compress_on);
            if (tokens.empty())
                continue;
            if (tokens.front() == "define")
            {
                if (tokens.size() > 2)
                {
                    std::string dec = tokens[1].substr(1, tokens[1].find('(', 0)-1);
                    if ("DECIMAL" == dec)
                    {
                        tokens[1] += "," + tokens[2];
                        tokens.pop_back();
                    }
                }
                if (tokens.size() != 2)
                    BOOST_FAIL("Malformed string '" << line << "' in the file '" << sDataFilename << "'");
                type = tokens[1];
                fixture.testAll(fixture.conn, data);
                data.clear();
            }
            else if (tokens.front() == "data")
            {
                if (tokens.size() != 3)
                    BOOST_FAIL("Malformed string '" << line << "' in the file '" << sDataFilename << "'");

                // Получаем значение, с которым надо будет сравнить значение из libslave
                slave_type checked_value;
                getValue(tokens[2], checked_value);

                Fixture::Line<slave_type> current;
                current.type = type;
                current.filename = sDataFilename;
                current.line = line;
                current.lineNumber = line_num;
                current.insert = tokens[1];
                current.expected = checked_value;
                data.push_back(current);
            }
            else if (tokens.front()[0] == ';')
                continue;
            else
                BOOST_FAIL("Unknown command '" << tokens.front() << "' in the file '" << sDataFilename << "' on line " << line_num);
        }
        fixture.testAll(fixture.conn, data);
        data.clear();
    }

    void testOneFilter(slave::EventKind filter)
    {
        Fixture f(filter);
        testOneType<boost::mpl::int_<MYSQL_INT>>(f);
    }

    void testOneFilterAllTypes(slave::EventKind filter)
    {
        Fixture f(filter);
        testOneType<boost::mpl::int_<MYSQL_INT>>(f);
        testOneType<boost::mpl::int_<MYSQL_BIGINT>>(f);
        testOneType<boost::mpl::int_<MYSQL_CHAR>>(f);
        testOneType<boost::mpl::int_<MYSQL_VARCHAR>>(f);
        testOneType<boost::mpl::int_<MYSQL_TINYTEXT>>(f);
        testOneType<boost::mpl::int_<MYSQL_TEXT>>(f);
        testOneType<boost::mpl::int_<MYSQL_DECIMAL>>(f);
        testOneType<boost::mpl::int_<MYSQL_BIT>>(f);
        testOneType<boost::mpl::int_<MYSQL_SET>>(f);
    }
}// anonymous-namespace

test_suite* init_unit_test_suite(int argc, char* argv[])
{
#define ADD_FIXTURE_TEST(testFunction) \
    framework::master_test_suite().add(BOOST_TEST_CASE([&]() { testFunction(); }))

    ADD_FIXTURE_TEST(test_HelloWorld);
    ADD_FIXTURE_TEST(test_StartStopPosition);
    ADD_FIXTURE_TEST(test_SetBinlogPos);
    ADD_FIXTURE_TEST(test_Disconnect);

#undef ADD_FIXTURE_TEST

    framework::master_test_suite().add(BOOST_TEST_CASE([&]() { testOneFilterAllTypes(slave::eAll); }));

#define ADD_FILTER_TEST(filter) \
    framework::master_test_suite().add(BOOST_TEST_CASE([&]() { testOneFilter(filter); }))

    ADD_FILTER_TEST(slave::eInsert);
    ADD_FILTER_TEST(slave::eUpdate);
    ADD_FILTER_TEST(slave::eDelete);
    ADD_FILTER_TEST(slave::eNone);
    ADD_FILTER_TEST(static_cast<slave::EventKind>(~static_cast<uint8_t>(slave::eInsert)));
    ADD_FILTER_TEST(static_cast<slave::EventKind>(~static_cast<uint8_t>(slave::eUpdate)));
    ADD_FILTER_TEST(static_cast<slave::EventKind>(~static_cast<uint8_t>(slave::eDelete)));

#undef ADD_FILTER_TEST

    return 0;
}
