#include <algorithm>
#include <array>
#include <cassert>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <future>
#include <mutex>
#include <string>

#include <reproc++/drain.hpp>
#include <reproc++/reproc.hpp>
#include <reproc++/run.hpp>
#include <sago/platform_folders.h>
#include <kissnet.hpp>

#include <api/file_utils.h>
#include <api/logger.h>
#include <api/process_utils.h>
#include <api/string_utils.h>

#include <api/osc/osc_sender.h>
#include <api/osc/udp.hh>
#include <api/osc/udp_osc_server.h>

#include <api/audio/audio_processor.h>
#include <api/scope_exit.h>
#include <api/sonicpi_api.h>

#ifndef __APPLE__
#include <crossguid/guid.hpp>
#endif

using namespace std::chrono;
using namespace oscpkt;

namespace SonicPi
{

#ifdef DEBUG
Logger logger{ LT::DBG };
#else
Logger logger{ LT::INFO };
#endif

namespace
{
const uint32_t ProcessWaitMilliseconds = 10000;
const uint32_t KillProcessMilliseconds = 2000;
const uint32_t TerminateProcessMilliseconds = 2000;

template <typename T>
std::map<T, T> vector_convert_to_pairs(const std::vector<T>& vals)
{
    std::map<T, T> pairs;
    auto itrFirst = vals.begin();
    while (itrFirst != vals.end())
    {
        auto itrSecond = itrFirst + 1;
        if (itrSecond != vals.end())
        {
            pairs[*itrFirst] = *itrSecond;
        }

        itrFirst = ++itrSecond;
    };
    return pairs;
}

} // namespace

SonicPiAPI::SonicPiAPI(IAPIClient* pClient, APIProtocol protocol, LogOption logOption)
    : m_pClient(pClient)
    , m_protocol(protocol)
    , m_logOption(logOption)
{
    assert(m_pClient);
}

fs::path SonicPiAPI::FindHomePath() const
{
    fs::path homePath;
    auto pszHome = std::getenv("SONIC_PI_HOME");
    if (pszHome != nullptr)
    {
        homePath = fs::path(pszHome);
    }

    // Check for home path existence and if not, use user documents path
    if (!fs::exists(homePath))
    {
        homePath = fs::path(sago::getDocumentsFolder()).parent_path();
    }

    // Final attempt at getting the folder; try to create it if possible
    if (!fs::exists(homePath))
    {
        std::error_code err;
        if (!fs::create_directories(homePath, err))
        {
            // Didn't exist, and failed to create it
            return fs::path();
        }
    }
    return homePath;
}

std::error_code SonicPiAPI::RunProcess(const std::vector<std::string>& args, std::string* pOutput)
{
    assert(!args.empty());

    // Wait for process, then terminate it
    reproc::stop_actions stop = {
        { reproc::stop::wait, reproc::milliseconds(ProcessWaitMilliseconds) },
        { reproc::stop::terminate, reproc::milliseconds(TerminateProcessMilliseconds) },
        { reproc::stop::kill, reproc::milliseconds(KillProcessMilliseconds) }
    };

    reproc::options options;
    options.stop = stop;

    reproc::process proc;
    std::error_code ec = proc.start(args, options);
    if (ec == std::errc::no_such_file_or_directory)
    {
        std::ostringstream str;
        str << "RunProcess - Program Not Found";
        if (!args.empty())
            str << " : " << args[0];
        LOG(ERR, str.str());
        return ec;
    }
    else if (ec)
    {
        LOG(ERR, "RunProcess - " << ec.message());
        return ec;
    }

    if (pOutput)
    {
        reproc::sink::string sink(*pOutput);
        ec = reproc::drain(proc, sink, sink);
        if (ec)
        {
            LOG(ERR, "RunProcess Draining - " << ec.message());
            return ec;
        }
    }

    // Call `process::stop` manually so we can access the exit status.
    int status = 0;
    std::tie(status, ec) = proc.stop(options.stop);
    if (ec)
    {
        LOG(ERR, "RunProcess - " << ec.message());
        return ec;
    }
    return ec;
}

std::shared_ptr<reproc::process> SonicPiAPI::StartProcess(const std::vector<std::string>& args)
{

    assert(!args.empty());

    auto spProcess = std::make_shared<reproc::process>();

    // auto yoProc = reproc::process();

    std::error_code ec = spProcess->start(args);
    LOG(INFO, "Started...");

    if (!ec)
    {
        // Success
        auto status = spProcess->wait(0s);
        LOG(DBG, "Process OK - " << status.second.message());
        return spProcess;
    }

    if (ec == std::errc::no_such_file_or_directory)
    {
        std::ostringstream str;
        str << "StartProcess - Program Not Found";
        if (!args.empty())
            str << " : " << args[0];
        LOG(ERR, str.str());

    }
    else if (ec)
    {
        LOG(ERR, "StartProcess - " << ec.message());
    }

    // Something went wrong. We've already logged an error
    // message, but also reset the shared pointer so that
    // the caller can see there's a problem too.
    spProcess.reset();
    return spProcess;
}

bool SonicPiAPI::StartBootDaemon()
{
    LOG(INFO, "Launching Sonic Pi Boot Daemon:");

    std::string output;
    std::vector<std::string> args;

    args.push_back(GetPath(SonicPiPath::RubyPath).string());
    args.push_back(GetPath(SonicPiPath::BootDaemonPath).string());

    std::ostringstream str;
    for (auto& arg : args)
    {
        str << arg << " ";
    }
    LOG(INFO, "Args: " << str.str());

    m_bootDaemonProcess = StartProcess(args);

    if (!m_bootDaemonProcess)
    {
        MessageInfo message;
        message.type = MessageType::StartupError;
        message.text = "The Boot Daemon could not be started!";

        m_pClient->Report(message);

        LOG(ERR, "Failed to start Boot Daemon!");
        return false;
    }

    LOG(INFO, "Attempting to read Boot Daemon output");

    // We need a mutex along with `output` to prevent the main thread and
    // background thread from modifying `output` at the same time (`std::string`
    // is not thread safe).
    uint8_t buffer[4096];
    auto res = m_bootDaemonProcess->read(reproc::stream::out, buffer, sizeof(buffer));

    int bytes_read = (int)res.first;
    std::error_code ec = res.second;

    if(ec || bytes_read < 0) {
      if(ec) {
        LOG(ERR, "Error reading ports via Boot Daemon STDOUT");
      } else {
        LOG(ERR, "Failed to read ports via Boot Daemon STDOUT. Bytes read: " + std::to_string(bytes_read));

    }
            return false;
    }

    std::string input_str(buffer, buffer + bytes_read);

    input_str = string_trim(input_str);
    auto ports = string_split(input_str, " ");
    std::transform(ports.begin(), ports.end(), ports.begin(), [](std::string& val) { return string_trim(val); });


    for(int i = 0 ; i < ports.size() ; i ++) {
      int port = std::stoi(ports[i]);
      LOG(INFO, "port: " + std::to_string(port));
    }

    if(ports.size() != 5) {
      LOG(ERR, "\nError. Was expecting 5 port numbers from the Daemon Booter. Got: " + std::to_string(ports.size()) + "\n");
      return false;
    }

    m_ports[SonicPiPortId::daemon] = std::stoi(ports[0]);
    m_ports[SonicPiPortId::gui_listen_to_server] = std::stoi(ports[1]);
    m_ports[SonicPiPortId::gui_send_to_server] = std::stoi(ports[2]);
    m_ports[SonicPiPortId::scsynth] = std::stoi(ports[3]);
    m_ports[SonicPiPortId::server_osc_cues] = std::stoi(ports[4]);

    m_spOscSender = std::make_shared<OscSender>(std::stoi(ports[2]));

    m_bootDaemonSock = std::make_shared<kissnet::tcp_socket>(kissnet::endpoint("127.0.0.1", m_ports[SonicPiPortId::daemon]));
    m_bootDaemonSock->connect();

    LOG(INFO, "Setting up Boot Daemon keep alive loop");
    m_bootDaemonSockPingLoopThread = std::thread([&]() {
      auto keep_alive_msg = std::string{ "keep-alive\n" };
      while(true)
      {
        LOG(DBG, "SND keep_alive");
        m_bootDaemonSock->send(reinterpret_cast<const std::byte*>(keep_alive_msg.c_str()), keep_alive_msg.size());
        LOG(DBG, "SND keep_alive sent");
        std::this_thread::sleep_for(1s);
      }
    });

    m_startServerTime = timer_start();
    return true;
}

SonicPiAPI::~SonicPiAPI()
{
    Shutdown();
}

void SonicPiAPI::Shutdown()
{
    LOG(INFO, "Shutdown");

    switch(m_state)
    {
    case State::Reset :
      LOG(INFO, "Shutting down with state: Reset");
      break;
    case State::Initializing :
      LOG(INFO, "Shutting down with state: Initializing");
      break;
    case State::Invalid :
      LOG(INFO, "Shutting down with state: Invalid");
      break;
    case State::Created :
      LOG(INFO, "Shutting down with state: Created");
      break;
    case State::Error :
      LOG(INFO, "Shutting down with state: Error");
      break;
    default :
      LOG(INFO, "Shutting down with unknown state!! Warning!");
    }

    if (m_state == State::Created || m_state == State::Invalid || m_state == State::Initializing)
    {
        LOG(INFO, "Resetting audio processor...");
        m_spAudioProcessor.reset();

        LOG(INFO, "Stopping OSC server...");
        StopOscServer();

        if (m_coutbuf)
        {
            std::cout.rdbuf(m_coutbuf); // reset to stdout before exiting
            m_coutbuf = nullptr;
        }
    }

    if (m_bootDaemonSock)
    {
        LOG(INFO, "Closing socket to Boot Daemon...");
        m_bootDaemonSock->close();
    } else {
      LOG(INFO, "Boot Daemon socket not found so no need to close...");
    }

    m_state = State::Reset;
    LOG(INFO, "API State set to: Reset...");

}

bool SonicPiAPI::StartOscServer()
{
    if (m_protocol == APIProtocol::UDP)

      {
        auto listenPort = GetPort(SonicPiPortId::gui_listen_to_server);

        m_spOscServer = std::make_shared<OscServerUDP>(m_pClient, std::make_shared<OscHandler>(m_pClient), listenPort);
        m_oscServerThread = std::thread([&]() {
            m_spOscServer->start();
            LOG(DBG, "Osc Server Thread Exiting");
        });
    }
    else
    {
        // TODO: TCP
        //sonicPiOSCServer = new SonicPiTCPOSCServer(this, handler);
        //sonicPiOSCServer->start();
    }
    return true;
}

bool SonicPiAPI::SendOSC(Message m)
{

    if (WaitUntilReady())
    {
        bool res = m_spOscSender->sendOSC(m);
        if (!res)
        {
            LOG(ERR, "Could Not Send OSC");
            return false;
        }
        return true;
    }

    return false;
}

bool SonicPiAPI::WaitUntilReady()
{
    if (m_state == State::Created)
    {
      return true;
    }

    int num_tries = 60;
    while (m_state != State::Created && num_tries > 0)
    {
        num_tries--;
        if (m_state == State::Error)
        {
          LOG(ERR, "Oh no, Spider Server got to an Error State whilst starting...");
          return false;
        }
        LOG(ERR, "Waiting Until Ready... " + std::to_string(num_tries));
        std::this_thread::sleep_for(1s);
    }

    if (num_tries < 1)
    {
      return false;
    } else {
      return true;
    }
}


bool SonicPiAPI::PingUntilServerCreated()
{
    LOG(INFO, "Pinging Spider Server until a response is received...");
    if (m_state == State::Created)
    {
        return true;
      LOG(ERR, "Error! No need to ping server as it's already created!");
    }

    if (m_state != State::Initializing)
    {
        LOG(ERR, "API is not in the initialisation state. Error!");
        return false;
    }

    int timeout = 60;
    LOG(INFO, "Waiting for Sonic Pi Spider Server to respond...");
    while (m_spOscServer->waitForServer() && timeout-- > 0)
    {
        std::this_thread::sleep_for(1s);
        LOG(INFO, ".");
        if (m_spOscServer->isIncomingPortOpen())
        {
            Message msg("/ping");
            msg.pushStr(m_guid);
            msg.pushStr("QtClient/1/hello");

            //bypass ::SendOSC as that needs to wait until ready
            //which is precisely what this message is attempting
            //to figure out!
            m_spOscSender->sendOSC(msg);
        }
    }

    if (!m_spOscServer->isServerStarted())
    {
        MessageInfo message;
        message.type = MessageType::StartupError;
        message.text = "Critical error! Could not connect to Sonic Pi Server.";

        m_pClient->Report(message);
        m_state = State::Error;
        return false;
    }
    else
    {
        auto time = timer_stop(m_startServerTime);
        LOG(INFO, "Sonic Pi Server connection established in " << time << "s");

        // Create the audio processor
        m_spAudioProcessor = std::make_shared<AudioProcessor>(m_pClient, GetPort(SonicPiPortId::scsynth));

        // All good
        m_state = State::Created;
        LOG(INFO, "API State set to: Created...");

        return true;
    }
}

// Initialize the API with the sonic pi root path (the folder containing the app folder)
bool SonicPiAPI::Init(const fs::path& root)
{

  m_osc_mtx.lock();

    if (m_state == State::Created)
    {
        MessageInfo message;
        message.type = MessageType::StartupError;
        message.text = "Call shutdown before Init!";

        m_pClient->Report(message);
        LOG(ERR, "Call shutdown before init!");
        return false;
    }


    // Start again, shutdown if we fail init
    m_state = State::Invalid;
    auto exitScope = sg::make_scope_guard([&]() {
        if (m_state == State::Invalid)
        {
            LOG(DBG, "Init failure, calling shutdown");
            Shutdown();
        }
    });

    // A new Guid for each initialization
#if defined(__APPLE__)
    m_guid = random_string(32);
#else
    m_guid = xg::newGuid().str();
#endif

    if (!fs::exists(root))
    {
        MessageInfo message;
        message.type = MessageType::StartupError;
        message.text = "Could not find root path: " + root.string();

        m_pClient->Report(message);
        return false;
    }

    if (!InitializePaths(root))
    {
      // oh no, something went wrong :-(
      return false;
    }

    // Make the log folder and check we can write to it.
    // This is /usr/home/.sonic-pi/log
    m_homeDirWriteable = true;
    auto logPath = GetPath(SonicPiPath::LogPath);
    if (!fs::exists(logPath))
    {
        std::error_code err;
        if (!fs::create_directories(logPath, err))
        {
            m_homeDirWriteable = false;
            LOG(INFO, "Home dir not writable: " << err.message());
        }
        else
        {
            std::ofstream fstream(logPath / ".writeTest");
            if (!fstream.is_open())
            {
                m_homeDirWriteable = false;
                LOG(INFO, "Home dir not writable!");
            }
        }
    }

    EnsurePathsAreCanonical();

    // Setup redirection of log from this app to our log file
    // stdout into ~/.sonic-pi/log/gui.log
    if (m_homeDirWriteable && (m_logOption == LogOption::File))
    {
        m_coutbuf = std::cout.rdbuf();
        m_stdlog.open(m_paths[SonicPiPath::GUILogPath].string().c_str());
        std::cout.rdbuf(m_stdlog.rdbuf());
    }

    LOG(INFO, "Welcome to Sonic Pi");
    LOG(INFO, "===================");

    if (m_homeDirWriteable) {
        LOG(INFO, "Home dir writable: ");
      } else {
        LOG(INFO, "Home dir NOT writable: ");
    }

    LOG(INFO, "Log PAth: " + GetPath(SonicPiPath::LogPath).string());


    // Start the Boot Daemon
    if (!StartBootDaemon())
    {
        return false;
    }

    // Start the OC Server
    if(!StartOscServer())
    {
        return false;
    }

    LOG(INFO, "API Init Started...");

    m_state = State::Initializing;
    LOG(INFO, "API State set to: Initializing...");

    m_osc_mtx.unlock();
    LOG(INFO, "Going to start pinging server...");
    m_pingerThread = std::thread([&]() {
        PingUntilServerCreated();
    });

    return true;
}

bool SonicPiAPI::InitializePaths(const fs::path& root)
{
    // sanitise and set app root path
    m_paths[SonicPiPath::RootPath] = fs::canonical(fs::absolute(root));

    // Sonic pi home directory
    m_paths[SonicPiPath::UserPath] = FindHomePath() / ".sonic-pi";

    // Set path to Ruby executable (system dependent)
#if defined(WIN32)
    m_paths[SonicPiPath::RubyPath] = m_paths[SonicPiPath::RootPath] / "app/server/native/ruby/bin/ruby.exe";
#else
    m_paths[SonicPiPath::RubyPath] = m_paths[SonicPiPath::RootPath] / "app/server/native/ruby/bin/ruby";
#endif
    if (!fs::exists(m_paths[SonicPiPath::RubyPath]))
    {
        m_paths[SonicPiPath::RubyPath] = "ruby";
    }

    // Set Ruby script paths
    m_paths[SonicPiPath::BootDaemonPath]      = m_paths[SonicPiPath::RootPath] / "app/server/ruby/bin/daemon.rb";
    m_paths[SonicPiPath::FetchUrlPath]        = m_paths[SonicPiPath::RootPath] / "app/server/ruby/bin/fetch-url.rb";

    // Set Log paths
    m_paths[SonicPiPath::LogPath] = m_paths[SonicPiPath::UserPath] / "log";
    m_paths[SonicPiPath::SpiderServerLogPath] = m_paths[SonicPiPath::LogPath] / "spider.log";
    m_paths[SonicPiPath::BootDaemonLogPath]   = m_paths[SonicPiPath::LogPath] / "daemon.log";
    m_paths[SonicPiPath::TauLogPath]          = m_paths[SonicPiPath::LogPath] / "tau.log";
    m_paths[SonicPiPath::SCSynthLogPath]      = m_paths[SonicPiPath::LogPath] / "scsynth.log";
    m_paths[SonicPiPath::GUILogPath]          = m_paths[SonicPiPath::LogPath] / "gui.log";

    // Set built-in samples path
    m_paths[SonicPiPath::SamplePath] = m_paths[SonicPiPath::RootPath] / "etc/samples/";

    // Sanity check for script existence
    const auto checkPaths = std::vector<SonicPiPath>{ SonicPiPath::FetchUrlPath, SonicPiPath::BootDaemonPath };
    for (const auto& check : checkPaths)
    {
        if (!fs::exists(m_paths[check]))
        {
            MessageInfo message;
            message.type = MessageType::StartupError;
            message.text = "Could not find script path: " + m_paths[check].string();

            m_pClient->Report(message);
            return false;
        }
    }

    return true;
}

void SonicPiAPI::EnsurePathsAreCanonical()
{
    std::for_each(m_paths.begin(), m_paths.end(), [this](auto& entry) {
        if (fs::exists(entry.second))
        {
            entry.second = fs::canonical(entry.second);
        }
        return entry;
    });
}

bool SonicPiAPI::TestAudio()
{
    // Just play a chord
    auto fileName = "d:/pi.rb";
    Message msg("/save-and-run-buffer");
    msg.pushStr(m_guid);
    msg.pushStr(fileName);
    msg.pushStr("play_chord [:c4, :e4, :g4]");
    msg.pushStr(fileName);
    bool res = SendOSC(msg);
    return res;
}

void SonicPiAPI::StopOscServer()
{
    // /*
    // * TODO: TCP
    // if(m_protocol == APIProtocol::TCP){
    //     clientSock->close();
    // }
    // */

    // Stop the osc server and hence the osc thread
    if (m_spOscServer)
    {
        LOG(DBG, "Stopping Osc Server...");
        m_spOscServer->stop();
    }

    // The server should have closed the osc channel; therefore we can join the thread
    if (m_oscServerThread.joinable())
    {
        LOG(DBG, "Waiting for Osc Server Thread...");
        m_oscServerThread.join();
        LOG(DBG, "Osc Server Thread done");
    }
    else
    {
        LOG(DBG, "Osc Server thread has already stopped");
    }
    m_spOscSender.reset();
}


const fs::path& SonicPiAPI::GetPath(SonicPiPath piPath)
{
    return m_paths[piPath];
}

const int& SonicPiAPI::GetPort(SonicPiPortId port)
{
    return m_ports[port];
}

std::string SonicPiAPI::GetLogs()
{
    auto logs = std::vector<fs::path>{ GetPath(SonicPiPath::SpiderServerLogPath),
        GetPath(SonicPiPath::BootDaemonLogPath),
        GetPath(SonicPiPath::TauLogPath),
        GetPath(SonicPiPath::SCSynthLogPath),
        GetPath(SonicPiPath::GUILogPath) };

    std::ostringstream str;
    for (auto& log : logs)
    {
        if (fs::exists(log))
        {
            auto contents = string_trim(file_read(log));
            if (!contents.empty())
            {
                str << log << ":" << std::endl
                    << contents << std::endl
                    << std::endl;
            }
        }
    }
    return str.str();
}

void SonicPiAPI::AudioProcessor_SetMaxFFTBuckets(uint32_t buckets)
{
    if (m_spAudioProcessor)
    {
        m_spAudioProcessor->SetMaxBuckets(buckets);
    }
}

void SonicPiAPI::AudioProcessor_Enable(bool enable)
{
    if (m_spAudioProcessor)
    {
        m_spAudioProcessor->Enable(enable);
    }
}

void SonicPiAPI::AudioProcessor_EnableFFT(bool enable)
{
    if (m_spAudioProcessor)
    {
        m_spAudioProcessor->EnableFFT(enable);
    }
}


void SonicPiAPI::AudioProcessor_ConsumedAudio()
{
    if (m_spAudioProcessor)
    {
        m_spAudioProcessor->SetConsumed(true);
    }
}


const std::string& SonicPiAPI::GetGuid() const
{
    return m_guid;
}

void SonicPiAPI::BufferNewLineAndIndent(int point_line, int point_index, int first_line, const std::string& code, const std::string& fileName, const std::string& id)
{
    Message msg("/buffer-newline-and-indent");
    msg.pushStr(id);
    msg.pushStr(fileName);
    msg.pushStr(code);
    msg.pushInt32(point_line);
    msg.pushInt32(point_index);
    msg.pushInt32(first_line);
    SendOSC(msg);
}

void SonicPiAPI::Run(const std::string& buffer, const std::string& text)
{
    Message msg("/save-and-run-buffer");
    msg.pushStr(m_guid);
    msg.pushStr(buffer);
    msg.pushStr(text);
    msg.pushStr(buffer);
    bool res = SendOSC(msg);
}

void SonicPiAPI::Stop()
{
    Message msg("/stop-all-jobs");
    msg.pushStr(m_guid);
    SendOSC(msg);
}

uint32_t SonicPiAPI::MaxWorkspaces() const
{
    return 10;
}

void SonicPiAPI::LoadWorkspaces()
{
    for (uint32_t i = 0; i < MaxWorkspaces(); i++)
    {
        Message msg("/load-buffer");
        msg.pushStr(m_guid);
        std::string s = "workspace_" + string_number_name(i);
        msg.pushStr(s);
        SendOSC(msg);
    }
}

void SonicPiAPI::SaveWorkspaces(const std::map<uint32_t, std::string>& workspaces)
{
    LOG(INFO, "Saving workspaces");

    for (uint32_t i = 0; i < MaxWorkspaces(); i++)
    {
        auto itrSpace = workspaces.find(i);
        if (itrSpace != workspaces.end())
        {
            Message msg("/save-buffer");
            msg.pushStr(m_guid);
            std::string s = "workspace_" + string_number_name(i);
            msg.pushStr(s);
            msg.pushStr(itrSpace->second);
            SendOSC(msg);
        }
    }

}

bool SonicPiAPI::SaveAndRunBuffer(const std::string& name, const std::string& text)
{
    std::string code = text;
    m_settings.Preprocess(code);

    Message msg("/save-and-run-buffer");
    msg.pushStr(m_guid);
    msg.pushStr(name);
    msg.pushStr(code);
    msg.pushStr(name);
    bool res = SendOSC(msg);
    if (!res)
    {
        return false;
    }
    return true;
}

const APISettings& SonicPiAPI::GetSettings() const
{
    return m_settings;
}

void SonicPiAPI::SetSettings(const APISettings& settings)
{
    m_settings = settings;
}

} // namespace SonicPi
