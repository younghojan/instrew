
#include "connection.h"

#include "cache.h"
#include "config.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <unordered_map>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <cstring>
#include <pwd.h>
#include <openssl/md5.h>
#include <sqlite3.h>

#define PATH_MAX 4096

namespace
{

    struct HexBuffer
    {
        const uint8_t *buf;
        size_t size;

        friend std::ostream &operator<<(std::ostream &os, HexBuffer const &self)
        {
            os << std::hex << std::setfill('0');
            for (size_t i = 0; i != self.size; i++)
                os << std::setw(2) << static_cast<int>(self.buf[i]);
            return os << std::dec;
        }
    };

    namespace Msg
    {
        enum Id
        {
#define INSTREW_MESSAGE_ID(id, name) name = id,

#include "instrew-protocol.inc"

#undef INSTREW_MESSAGE_ID
        };
        struct Hdr
        {
            uint32_t id;
            int32_t sz;
        } __attribute__((packed));
    }

    class SQLiteDatabase
    {
    public:
        SQLiteDatabase(const std::string &dbPath) : dbPath_(dbPath), db_(nullptr)
        {
            int rc = sqlite3_open(dbPath_.c_str(), &db_);
            if (rc != SQLITE_OK)
            {
                std::cerr << "Database does not exist. Creating..." << std::endl;

                rc = sqlite3_open(dbPath_.c_str(), &db_);
                if (rc != SQLITE_OK)
                {
                    std::cerr << "Error creating database: " << sqlite3_errmsg(db_) << std::endl;
                    sqlite3_close(db_);
                    db_ = nullptr;
                }
                else
                {
                    std::cout << "Database created successfully." << std::endl;
                }
            }

            // Check if table 'programs' exists, and create it if not
            if (tableExists("programs") == false)
            {
                std::cout << "Table 'programs' does not exist. Creating..." << std::endl;
                std::string createTableSQL = "CREATE TABLE IF NOT EXISTS programs (id INTEGER PRIMARY KEY AUTOINCREMENT, hash TEXT, dir_path TEXT);";
                if (executeSQL(createTableSQL))
                {
                    std::cout << "Table 'programs' created successfully." << std::endl;
                }
                else
                {
                    std::cerr << "Error creating table 'programs': " << sqlite3_errmsg(db_) << std::endl;
                }
            }
        }

        ~SQLiteDatabase()
        {
            if (db_ != nullptr)
            {
                sqlite3_close(db_);
            }
        }

        bool executeSQL(const std::string &sql)
        {
            if (db_ == nullptr)
            {
                std::cerr << "Error: Database is not open." << std::endl;
                return false;
            }

            char *errMsg = nullptr;
            int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errMsg);
            if (rc != SQLITE_OK)
            {
                std::cerr << "Error executing SQL: " << errMsg << std::endl;
                sqlite3_free(errMsg);
                return false;
            }

            return true;
        }

        bool executeSelect(const std::string &sql, std::vector<std::vector<std::string>> &results)
        {
            if (db_ == nullptr)
            {
                std::cerr << "Error: Database is not open." << std::endl;
                return false;
            }

            sqlite3_stmt *stmt = nullptr;
            int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
            if (rc != SQLITE_OK)
            {
                std::cerr << "Error preparing SQL statement: " << sqlite3_errmsg(db_) << std::endl;
                sqlite3_finalize(stmt);
                return false;
            }

            int cols = sqlite3_column_count(stmt);
            results.clear();

            while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
            {
                std::vector<std::string> row;
                for (int col = 0; col < cols; col++)
                {
                    const unsigned char *value = sqlite3_column_text(stmt, col);
                    row.push_back(std::string(reinterpret_cast<const char *>(value)));
                }
                results.push_back(row);
            }

            sqlite3_finalize(stmt);

            if (rc != SQLITE_DONE)
            {
                std::cerr << "Error executing SQL: " << sqlite3_errmsg(db_) << std::endl;
                return false;
            }

            return true;
        }

    private:
        std::string dbPath_;
        sqlite3 *db_;

        bool tableExists(const std::string &tableName)
        {
            std::string checkTableSQL = "SELECT name FROM sqlite_master WHERE type='table' AND name='" + tableName + "';";
            std::vector<std::vector<std::string>> results;
            if (executeSelect(checkTableSQL, results) && results.size() > 0)
            {
                return true;
            }
            return false;
        }
    };

    class Conn
    {
    private:
        std::FILE *file_rd;
        std::FILE *file_wr;
        Msg::Hdr wr_hdr;
        Msg::Hdr recv_hdr;

        Conn(std::FILE *file_rd, std::FILE *file_wr)
            : file_rd(file_rd), file_wr(file_wr), recv_hdr{} {}

    public:
        static Conn CreateFork(char *argv0, size_t uargc, const char *const *uargv, const bool debug,
                               const std::string rerunner_path, SQLiteDatabase db, std::string &dir_path);

        Msg::Id RecvMsg()
        {
            assert(recv_hdr.sz == 0 && "unread message parts");
            if (!std::fread(&recv_hdr, sizeof(recv_hdr), 1, file_rd))
                return Msg::C_EXIT; // probably EOF
            return static_cast<Msg::Id>(recv_hdr.id);
        }

        void Read(void *buf, size_t size)
        {
            if (static_cast<size_t>(recv_hdr.sz) < size)
                assert(false && "message too small");
            if (!std::fread(buf, size, 1, file_rd))
                assert(false && "unable to read msg content");
            recv_hdr.sz -= size;
        }

        template <typename T>
        T Read()
        {
            T t;
            Read(&t, sizeof(t));
            return t;
        }

        void SendMsgHdr(Msg::Id id, size_t size)
        {
            assert(size <= INT32_MAX);
            wr_hdr = Msg::Hdr{id, static_cast<int32_t>(size)};
            if (!std::fwrite(&wr_hdr, sizeof(wr_hdr), 1, file_wr))
                assert(false && "unable to write msg hdr");
        }

        void Write(const void *buf, size_t size)
        {
            if (size > 0 && !std::fwrite(buf, size, 1, file_wr))
                assert(false && "unable to write msg content");
            wr_hdr.sz -= size;
            if (wr_hdr.sz == 0)
                std::fflush(file_wr);
        }

        void Sendfile(int fd, size_t size)
        {
            std::fflush(file_wr);
            while (size)
            {
                ssize_t cnt = sendfile(fileno(file_wr), fd, nullptr, size);
                if (cnt < 0)
                    assert(false && "unable to write msg content (sendfile)");
                size -= cnt;
            }
            wr_hdr.sz -= size;
        }

        template <typename T>
        void SendMsg(Msg::Id id, const T &val)
        {
            SendMsgHdr(id, sizeof(T));
            Write(&val, sizeof(T));
        }
    };

    Conn Conn::CreateFork(char *argv0, size_t uargc, const char *const *uargv, const bool debug,
                          const std::string rerunner_path, SQLiteDatabase db, std::string &dir_path)
    {
        if (std::filesystem::exists(rerunner_path)) // if rerunner exists
        {
            // compute MD5
            std::ifstream file(uargv[0], std::ios::binary);
            MD5_CTX ctx;
            MD5_Init(&ctx);
            char buffer[BUFSIZ];
            while (file.read(buffer, sizeof(buffer)) || file.gcount())
            {
                MD5_Update(&ctx, buffer, file.gcount());
            }
            unsigned char hash[MD5_DIGEST_LENGTH];
            MD5_Final(hash, &ctx);

            std::stringstream ss;
            ss << std::hex << std::setfill('0');
            for (int i = 0; i < MD5_DIGEST_LENGTH; ++i)
            {
                ss << std::setw(2) << static_cast<int>(hash[i]);
            }
            std::string MD5 = ss.str();

            // check if translated before
            std::string selectSQL = "SELECT dir_path FROM programs WHERE hash = '" + MD5 + "';";
            std::vector<std::vector<std::string>> results;
            if (db.executeSelect(selectSQL, results) && results.size())
            {
                // we want arguments of last execution
                dir_path = results[0][0];
                std::string user_args_path = dir_path[dir_path.length() - 1] == '/' ? dir_path + "user_args" : dir_path + "/user_args";
                std::ifstream user_args_file(user_args_path);
                std::vector<std::string> args;
                std::string line;
                std::getline(user_args_file, line);
                std::istringstream iss(line);
                std::string substring;
                while (iss >> substring)
                {
                    args.emplace_back(substring);
                }

                int flag = 0;
                if (std::stoi(args[0]) + 1 == uargc) // same argc
                {
                    for (int i = 1; i < static_cast<int>(args.size()); i++) // Read in the parameters of the last execution
                    {
                        if (args[i].compare(uargv[i - 1]) == 0) // same as this time
                        {
                            flag = 1;
                        }
                        else
                        {
                            break;
                        }
                    }

                    if (flag) // correct args
                    {
                        std::string cmd = rerunner_path + " " + dir_path;
                        system(cmd.c_str());
                        exit(0);
                    }
                }
            }
            else
            {
                dir_path = rerunner_path;
                dir_path.erase(dir_path.find_last_of('/') + 1);
                dir_path += MD5;
                std::error_code ec;
                std::filesystem::create_directories(dir_path, ec);
            }
        }

        int pipes[4];
        int ret = pipe2(&pipes[0], 0);
        if (ret < 0)
        {
            perror("pipe2");
            std::exit(1);
        }
        ret = pipe2(&pipes[2], 0);
        if (ret < 0)
        {
            perror("pipe2");
            std::exit(1);
        }

        uint64_t raw_fds = (uint64_t)pipes[0] | (uint64_t)pipes[3] << 32;
        std::string client_config;
        for (; raw_fds; raw_fds >>= 3)
            client_config.append(1, '0' + (raw_fds & 7));
        std::vector<const char *> exec_args;
        exec_args.reserve(uargc + 3);
        exec_args.push_back(argv0);
        exec_args.push_back(client_config.c_str());
        for (size_t i = 0; i < uargc; i++)
            exec_args.push_back(uargv[i]);
        exec_args.push_back(nullptr);

        int memfd;
        if (debug)
        {
            char client_path[PATH_MAX] = "";
            readlink("/proc/self/exe", client_path, sizeof(client_path));
            strcat(client_path, "-client");
            memfd = open(client_path, O_RDONLY);
            if (memfd < 0)
            {
                perror("open");
                std::exit(1);
            }
        }
        else
        {
            static const unsigned char instrew_stub[] = {
#include "client.inc"
            };

            memfd = memfd_create("instrew_stub", MFD_CLOEXEC);
            if (memfd < 0)
            {
                perror("memfd_create");
                std::exit(1);
            }
            size_t written = 0;
            size_t total = sizeof(instrew_stub);
            while (written < total)
            {
                auto wres = write(memfd, instrew_stub + written, total - written);
                if (wres < 0)
                {
                    perror("write");
                    std::exit(1);
                }
                written += wres;
            }
        }

        pid_t forkres = fork();
        if (forkres < 0)
        {
            perror("fork");
            std::exit(1);
        }
        else if (forkres > 0)
        {
            close(pipes[1]);
            close(pipes[2]);

            fexecve(memfd, const_cast<char *const *>(&exec_args[0]), environ);
            perror("fexecve");
            std::exit(1);
        }
        close(memfd);
        close(pipes[0]);
        close(pipes[3]);

        return {fdopen(pipes[2], "rb"), fdopen(pipes[1], "wb")};
    }

    class RemoteMemory
    {
    private:
        const static size_t PG_SIZE = 0x1000;
        using Page = std::array<uint8_t, PG_SIZE>;
        std::unordered_map<uint64_t, std::unique_ptr<Page>> page_cache;
        Conn &conn;

    public:
        explicit RemoteMemory(Conn &c) : conn(c) {}

    private:
        Page *GetPage(size_t page_addr)
        {
            const auto &page_it = page_cache.find(page_addr);
            if (page_it != page_cache.end())
                return page_it->second.get();

            struct
            {
                uint64_t addr;
                size_t buf_sz;
            } send_buf{page_addr, PG_SIZE};
            conn.SendMsg(Msg::S_MEMREQ, send_buf);

            Msg::Id msgid = conn.RecvMsg();
            if (msgid != Msg::C_MEMBUF)
                return nullptr;

            auto page = std::make_unique<Page>();
            conn.Read(page->data(), page->size());
            uint8_t failed = conn.Read<uint8_t>();
            if (failed)
                return nullptr;

            page_cache[page_addr] = std::move(page);

            return page_cache[page_addr].get();
        };

    public:
        size_t Get(size_t start, size_t end, uint8_t *buf)
        {
            size_t start_page = start & ~(PG_SIZE - 1);
            size_t end_page = end & ~(PG_SIZE - 1);
            size_t bytes_written = 0;
            for (size_t cur = start_page; cur <= end_page; cur += PG_SIZE)
            {
                Page *page = GetPage(cur);
                if (!page)
                    break;
                size_t start_off = cur < start ? (start & (PG_SIZE - 1)) : 0;
                size_t end_off = cur + PG_SIZE > end ? (end & (PG_SIZE - 1)) : PG_SIZE;
                std::copy(page->data() + start_off, page->data() + end_off, buf + bytes_written);
                bytes_written += end_off - start_off;
            }
            return bytes_written;
        }
    };
} // end namespace

struct IWConnection
{
    const struct IWFunctions *fns;
    InstrewConfig &cfg;
    Conn &conn;

    IWServerConfig iwsc;
    IWClientConfig iwcc;
    bool need_iwcc{};

    RemoteMemory remote_memory;
    instrew::Cache cache;

    std::string dir_path;

    IWConnection(const struct IWFunctions *fns, InstrewConfig &cfg, Conn &conn, const std::string dir_path)
        : fns(fns), cfg(cfg), conn(conn), remote_memory(conn), dir_path(dir_path) {}

private:
    FILE *OpenObjDump(uint64_t addr)
    {
        std::stringstream debug_out1_name;
        debug_out1_name << std::hex << addr;
        return std::fopen((dir_path + "/" + debug_out1_name.str()).c_str(), "wb");
    }

public:
    void ArgsDump(size_t user_argc, const char *const *user_args)
    {
        std::stringstream user_args_filename;
        user_args_filename << std::hex << "user_args";
        FILE *fp = std::fopen((dir_path + "/" + user_args_filename.str()).c_str(), "wb");
        fprintf(fp, "%ld ", user_argc - 1);
        for (size_t i = 0; i < user_argc; i++)
        {
            fprintf(fp, "%s ", user_args[i]);
        }
    }

    bool CacheProbe(uint64_t addr, const uint8_t *hash)
    {
        (void)addr;
        auto res = cache.Get(hash);
        if (res.first < 0)
            return false;
        conn.SendMsgHdr(Msg::S_OBJECT, res.second);
        conn.Sendfile(res.first, res.second);
        close(res.first);
        return true;
    }

    void SendObject(uint64_t addr, const void *data, size_t size,
                    const uint8_t *hash)
    {
        if (need_iwcc)
        {
            conn.SendMsg(Msg::S_INIT, iwcc);
            need_iwcc = false;
        }
        conn.SendMsgHdr(Msg::S_OBJECT, size);
        conn.Write(data, size);
        if (FILE *df = OpenObjDump(addr))
        {
            std::fwrite(data, size, 1, df);
            std::fclose(df);
        }
        if (hash)
            cache.Put(hash, size, static_cast<const char *>(data));
    }

    int Run()
    {
        if (conn.RecvMsg() != Msg::C_INIT)
        {
            std::cerr << "error: expected C_INIT message" << std::endl;
            return 1;
        }
        iwsc = conn.Read<IWServerConfig>();

        ArgsDump(cfg.user_argc, cfg.user_args);

        // In mode 0, we need to respond with a client config.
        need_iwcc = iwsc.tsc_server_mode == 0;

        cache = instrew::Cache(cfg);

        IWState *state = fns->init(this, cfg);
        if (need_iwcc)
            SendObject(0, "", 0, nullptr); // this will send the client config

        while (true)
        {
            Msg::Id msgid = conn.RecvMsg();
            if (msgid == Msg::C_EXIT)
            {
                fns->finalize(state);
                state = nullptr;
                return 0;
            }
            else if (msgid == Msg::C_TRANSLATE)
            {
                auto addr = conn.Read<uint64_t>();
                fns->translate(state, addr);
            }
            else
            {
                std::cerr << "unexpected msg " << msgid << std::endl;
                return 1;
            }
        }
    }
};

const struct IWServerConfig *iw_get_sc(IWConnection *iwc)
{
    return &iwc->iwsc;
}

struct IWClientConfig *iw_get_cc(IWConnection *iwc)
{
    return &iwc->iwcc;
}

size_t iw_readmem(IWConnection *iwc, uintptr_t addr, size_t len, uint8_t *buf)
{
    return iwc->remote_memory.Get(addr, len, buf);
}

bool iw_cache_probe(IWConnection *iwc, uintptr_t addr, const uint8_t *hash)
{
    return iwc->CacheProbe(addr, hash);
}

void iw_sendobj(IWConnection *iwc, uintptr_t addr, const void *data,
                size_t size, const uint8_t *hash)
{
    iwc->SendObject(addr, data, size, hash);
}

int iw_run_server(const struct IWFunctions *fns, int argc, char **argv)
{
    InstrewConfig cfg(argc - 1, argv + 1);
    SQLiteDatabase db("md5.db");
    std::string dir_path;
    Conn conn = Conn::CreateFork(argv[0], cfg.user_argc, cfg.user_args, cfg.debug, cfg.rerunner, db, dir_path);
    IWConnection iwc{fns, cfg, conn, dir_path};
    return iwc.Run();
}
