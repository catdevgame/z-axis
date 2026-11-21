#include "runtime/save_store.h"
#include "runtime/frame_trace.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace zx::saves
{
    namespace
    {
        void setError(std::string *error, const std::string &text)
        {
            if (error)
                *error = text;
        }

        // Writes and flushes to disk (not just to the OS cache), so a crash
        // right after the rename cannot leave a renamed but empty file.
        bool writeAndSync(const std::filesystem::path &file, const uint8_t *data, size_t size)
        {
#if defined(_WIN32)
            FILE *out = _wfopen(file.c_str(), L"wb");
#else
            FILE *out = std::fopen(file.c_str(), "wb");
#endif
            if (!out)
                return false;
            bool ok = size == 0 || std::fwrite(data, 1, size, out) == size;
            ok = std::fflush(out) == 0 && ok;
#if defined(_WIN32)
            ok = _commit(_fileno(out)) == 0 && ok;
#else
            ok = fsync(fileno(out)) == 0 && ok;
#endif
            return std::fclose(out) == 0 && ok;
        }

        void syncFolder(const std::filesystem::path &folder)
        {
#if !defined(_WIN32)
            const int fd = open(folder.c_str(), O_RDONLY);
            if (fd >= 0)
            {
                fsync(fd);
                close(fd);
            }
#else
            (void)folder;
#endif
        }
    }

    std::string safeFileName(const std::string &name, const std::string &fallback)
    {
        std::string out = name;
        for (char &c : out)
        {
            const unsigned char u = static_cast<unsigned char>(c);
            const bool alnum = (u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z');
            if (!(alnum || c == ' ' || c == '-' || c == '_' || c == '.'))
                c = '_';
        }
        while (!out.empty() && (out.front() == '.' || out.front() == ' '))
            out.erase(out.begin());
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        return out.empty() ? fallback : out;
    }

    uint32_t byteSum(const uint8_t *data, size_t size)
    {
        uint32_t sum = 0;
        for (size_t i = 0; i < size; ++i)
            sum += data[i];
        return sum;
    }

    std::filesystem::path backupPath(const std::filesystem::path &file)
    {
        std::filesystem::path out = file;
        out += ".bak";
        return out;
    }

    std::filesystem::path tempPath(const std::filesystem::path &file)
    {
        std::filesystem::path out = file;
        out += ".tmp";
        return out;
    }

    bool writeFileAtomic(const std::filesystem::path &file, const uint8_t *data, size_t size, std::string *error)
    {
        zx::ftrace::Scope traceWrite(zx::ftrace::kFileWrite, zx::ftrace::kFileWrite + 100);
        std::error_code ec;
        const std::filesystem::path folder = file.parent_path();
        if (!folder.empty())
        {
            std::filesystem::create_directories(folder, ec);
            if (ec)
            {
                setError(error, "cannot create " + folder.string() + ": " + ec.message());
                return false;
            }
        }
        const std::filesystem::path temp = tempPath(file);
        if (!writeAndSync(temp, data, size))
        {
            std::filesystem::remove(temp, ec);
            setError(error, "cannot write " + temp.string());
            return false;
        }
        if (std::filesystem::exists(file, ec))
        {
            std::filesystem::rename(file, backupPath(file), ec);
            if (ec)
            {
                std::filesystem::remove(temp, ec);
                setError(error, "cannot keep the previous save as " + backupPath(file).string() + ": " + ec.message());
                return false;
            }
        }
        std::filesystem::rename(temp, file, ec);
        if (ec)
        {
            setError(error, "cannot rename " + temp.string() + " to " + file.string() + ": " + ec.message());
            return false;
        }
        if (!folder.empty())
            syncFolder(folder);
        return true;
    }

    bool readFile(const std::filesystem::path &file, Bytes &out)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in)
            return false;
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size < 0)
            return false;
        in.seekg(0, std::ios::beg);
        out.resize(static_cast<size_t>(size));
        if (size > 0)
            in.read(reinterpret_cast<char *>(out.data()), size);
        return static_cast<bool>(in) || size == 0;
    }

    namespace
    {
        // The background writer of writeFileAtomicLater: one thread, started
        // on the first request, drained and joined at exit.
        class Writer
        {
        public:
            ~Writer()
            {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_stop = true;
                }
                m_cv.notify_all();
                if (m_thread.joinable())
                    m_thread.join();
            }

            void post(const std::filesystem::path &file, Bytes data, const std::string &what)
            {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    bool replaced = false;
                    for (Request &r : m_queue)
                        if (r.file == file)
                        {
                            r.data = std::move(data);
                            r.what = what;
                            replaced = true;
                            break;
                        }
                    if (!replaced)
                        m_queue.push_back(Request{file, std::move(data), what});
                    if (!m_thread.joinable())
                        m_thread = std::thread([this]()
                                               { run(); });
                }
                m_cv.notify_all();
            }

            void flush()
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this]()
                          { return m_queue.empty() && !m_busy; });
            }

        private:
            struct Request
            {
                std::filesystem::path file;
                Bytes data;
                std::string what;
            };

            void run()
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                for (;;)
                {
                    m_cv.wait(lock, [this]()
                              { return m_stop || !m_queue.empty(); });
                    if (m_queue.empty())
                        return; // stopping, nothing left
                    Request r = std::move(m_queue.front());
                    m_queue.pop_front();
                    m_busy = true;
                    lock.unlock();
                    std::string error;
                    if (!writeFileAtomic(r.file, r.data.data(), r.data.size(), &error))
                        std::cerr << (r.what.empty() ? std::string("[saves]") : r.what) << ": " << error << std::endl;
                    lock.lock();
                    m_busy = false;
                    m_cv.notify_all();
                }
            }

            std::mutex m_mutex;
            std::condition_variable m_cv;
            std::deque<Request> m_queue;
            bool m_busy = false;
            bool m_stop = false;
            std::thread m_thread;
        };

        Writer &writer()
        {
            static Writer w;
            return w;
        }
    }

    void writeFileAtomicLater(const std::filesystem::path &file, Bytes data, const std::string &what)
    {
        // ZX_SAVES_SYNC=1: on the calling thread (as before; for measurements).
        static const bool sync = []
        {
            const char *v = std::getenv("ZX_SAVES_SYNC");
            return v && v[0] == '1';
        }();
        if (sync)
        {
            std::string error;
            if (!writeFileAtomic(file, data.data(), data.size(), &error))
                std::cerr << (what.empty() ? std::string("[saves]") : what) << ": " << error << std::endl;
            return;
        }
        writer().post(file, std::move(data), what);
    }

    void flushWrites()
    {
        writer().flush();
    }

    ReadResult readWithBackup(const std::filesystem::path &file, const Validator &valid)
    {
        flushWrites();
        ReadResult result;
        bool anyExists = false;
        for (const std::filesystem::path &candidate : {file, backupPath(file)})
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(candidate, ec))
                continue;
            anyExists = true;
            Bytes data;
            if (!readFile(candidate, data) || (valid && !valid(data)))
                continue;
            result.status = candidate == file ? ReadStatus::Ok : ReadStatus::FromBackup;
            result.data = std::move(data);
            return result;
        }
        result.status = anyExists ? ReadStatus::Invalid : ReadStatus::Missing;
        return result;
    }

    std::vector<ImportedSave> importFromCardFolders(const std::vector<std::filesystem::path> &cardFolders,
                                                    const ImportTarget &target, const Validator &valid,
                                                    std::string *error)
    {
        std::vector<ImportedSave> imported;
        std::vector<Bytes> seen;
        for (const std::filesystem::path &card : cardFolders)
        {
            std::error_code ec;
            if (!std::filesystem::is_directory(card, ec))
                continue;
            std::vector<std::string> folders;
            for (const auto &entry : std::filesystem::directory_iterator(card, ec))
                if (entry.is_directory(ec))
                    folders.push_back(entry.path().filename().string());
            std::sort(folders.begin(), folders.end());
            for (const std::string &name : folders)
            {
                const std::filesystem::path source = card / name / name;
                Bytes data;
                if (!std::filesystem::is_regular_file(source, ec) || !readFile(source, data) || (valid && !valid(data)))
                    continue;
                if (std::find(seen.begin(), seen.end(), data) != seen.end())
                    continue;
                const std::filesystem::path to = target(name);
                if (to.empty() || std::filesystem::exists(to, ec))
                    continue;
                std::string writeError;
                if (!writeFileAtomic(to, data.data(), data.size(), &writeError))
                {
                    setError(error, writeError);
                    continue;
                }
                seen.push_back(std::move(data));
                imported.push_back({source, to});
            }
        }
        return imported;
    }
}
