/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    Copyright (c) 2004 Stephan Kulow <coolo@suse.de>
                  2002, 2003 by Martin Pool <mbp@samba.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "config.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>


#ifdef __FreeBSD__
// Grmbl  Why is this needed?  We don't use readv/writev
#include <sys/uio.h>
#endif

#include <fcntl.h>
#include <signal.h>
#include <limits.h>
#include <assert.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <map>
#include <algorithm>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <vector>

#include <comm.h>
#include "client.h"
#include "tempfile.h"
#include "md5.h"
#include "services/util.h"

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif

namespace
{

struct CharBufferDeleter {
    char *buf;
    CharBufferDeleter(char *b) : buf(b) {}
    ~CharBufferDeleter() {
        free(buf);
    }
};

}

using namespace std;

std::string remote_daemon;

Environments
parse_icecc_version(const string &target_platform, const string &prefix)
{
    Environments envs;

    string icecc_version = getenv("ICECC_VERSION");
    assert(!icecc_version.empty());

    // free after the C++-Programming-HOWTO
    string::size_type lastPos = icecc_version.find_first_not_of(',', 0);
    string::size_type pos     = icecc_version.find_first_of(',', lastPos);
    bool def_targets = icecc_version.find('=') != string::npos;

    list<string> platforms;

    while (pos != string::npos || lastPos != string::npos) {
        string couple = icecc_version.substr(lastPos, pos - lastPos);
        string platform = target_platform;
        string version = couple;
        string::size_type colon = couple.find(':');

        if (colon != string::npos) {
            platform = couple.substr(0, colon);
            version = couple.substr(colon + 1, couple.length());
        }

        // Skip delimiters.  Note the "not_of"
        lastPos = icecc_version.find_first_not_of(',', pos);
        // Find next "non-delimiter"
        pos = icecc_version.find_first_of(',', lastPos);

        if (def_targets) {
            colon = version.find('=');

            if (colon != string::npos) {
                if (prefix != version.substr(colon + 1, version.length())) {
                    continue;
                }

                version = version.substr(0, colon);
            } else if (!prefix.empty()) {
                continue;
            }
        }

        if (find(platforms.begin(), platforms.end(), platform) != platforms.end()) {
            log_error() << "there are two environments for platform " << platform << " - ignoring " << version << endl;
            continue;
        }

        if (::access(version.c_str(), R_OK)) {
            log_error() << "$ICECC_VERSION has to point to an existing file to be installed " << version << endl;
            continue;
        }

        struct stat st;

        if (lstat(version.c_str(), &st) || !S_ISREG(st.st_mode) || st.st_size < 500) {
            log_error() << "$ICECC_VERSION has to point to an existing file to be installed " << version << endl;
            continue;
        }

        envs.push_back(make_pair(platform, version));
        platforms.push_back(platform);
    }

    return envs;
}

static bool
endswith(const string &orig, const char *suff, string &ret)
{
    size_t len = strlen(suff);

    if (orig.size() > len && orig.substr(orig.size() - len) == suff) {
        ret = orig.substr(0, orig.size() - len);
        return true;
    }

    return false;
}

static Environments
rip_out_paths(const Environments &envs, map<string, string> &version_map, map<string, string> &versionfile_map)
{
    version_map.clear();

    Environments env2;

    static const char *suffs[] = { ".tar.bz2", ".tar.gz", ".tar", ".tgz", NULL };

    string versfile;

    for (Environments::const_iterator it = envs.begin(); it != envs.end(); ++it) {
        for (int i = 0; suffs[i] != NULL; i++)
            if (endswith(it->second, suffs[i], versfile)) {
                versionfile_map[it->first] = it->second;
                versfile = find_basename(versfile);
                version_map[it->first] = versfile;
                env2.push_back(make_pair(it->first, versfile));
            }
    }

    return env2;
}


string
get_absfilename(const string &_file)
{
    string file;

    if (_file.empty()) {
        return _file;
    }

    if (_file.at(0) != '/') {
        static std::vector<char> buffer(1024);

        while (getcwd(&buffer[0], buffer.size() - 1) == 0 && errno == ERANGE) {
            buffer.resize(buffer.size() + 1024);
        }

        file = string(&buffer[0]) + '/' + _file;
    } else {
        file = _file;
    }

    string::size_type idx = file.find("/..");

    while (idx != string::npos) {
        file.replace(idx, 3, "/");
        idx = file.find("/..");
    }

    idx = file.find("/./");

    while (idx != string::npos) {
        file.replace(idx, 3, "/");
        idx = file.find("/./");
    }

    idx = file.find("//");

    while (idx != string::npos) {
        file.replace(idx, 2, "/");
        idx = file.find("//");
    }

    return file;
}

static UseCSMsg *get_server(MsgChannel *local_daemon)
{
    Msg *umsg = local_daemon->get_msg(4 * 60);

    if (!umsg || umsg->type != M_USE_CS) {
        log_warning() << "replied not with use_cs " << (umsg ? (char)umsg->type : '0')  << endl;
        delete umsg;
        throw client_error(1, "Error 1 - expected use_cs reply, but got something else");
    }

    UseCSMsg *usecs = dynamic_cast<UseCSMsg *>(umsg);
    return usecs;
}

static void check_for_failure(Msg *msg, MsgChannel *cserver)
{
    if (msg && msg->type == M_STATUS_TEXT) {
        log_error() << "Remote status (compiled on " << cserver->name << "): "
                    << static_cast<StatusTextMsg*>(msg)->text << endl;
        throw client_error(23, "Error 23 - Remote status (compiled on " + cserver->name + ")\n" +
                                 static_cast<StatusTextMsg*>(msg)->text );
    }
}

static void write_server_cpp(int cpp_fd, MsgChannel *cserver)
{
    unsigned char buffer[100000]; // some random but huge number
    off_t offset = 0;
    size_t uncompressed = 0;
    size_t compressed = 0;

    do {
        ssize_t bytes;

        do {
            bytes = read(cpp_fd, buffer + offset, sizeof(buffer) - offset);

            if (bytes < 0 && (errno == EINTR || errno == EAGAIN)) {
                continue;
            }

            if (bytes < 0) {
                log_perror("reading from cpp_fd");
                close(cpp_fd);
                throw client_error(16, "Error 16 - error reading local cpp file");
            }

            break;
        } while (1);

        offset += bytes;

        if (!bytes || offset == sizeof(buffer)) {
            if (offset) {
                FileChunkMsg fcmsg(buffer, offset);

                if (!cserver->send_msg(fcmsg)) {
                    Msg *m = cserver->get_msg(2);
                    check_for_failure(m, cserver);

                    log_error() << "write of source chunk to host "
                                << cserver->name.c_str() << endl;
                    log_perror("failed ");
                    close(cpp_fd);
                    throw client_error(15, "Error 15 - write to host failed");
                }

                uncompressed += fcmsg.len;
                compressed += fcmsg.compressed;
                offset = 0;
            }

            if (!bytes) {
                break;
            }
        }
    } while (1);

    if (compressed)
        trace() << "sent " << compressed << " bytes (" << (compressed * 100 / uncompressed) <<
                "%)" << endl;

    close(cpp_fd);
}

static void receive_fd(const int obj_fd, MsgChannel* cserver)
{
    Msg* msg = 0;
    size_t uncompressed = 0;
    size_t compressed = 0;

    while (1) {
        delete msg;

        msg = cserver->get_msg(40);

        if (!msg) {   // the network went down?
            throw client_error(19, "Error 19 - (network failure?)");
        }

        check_for_failure(msg, cserver);

        if (msg->type == M_END) {
            break;
        }

        if (msg->type != M_FILE_CHUNK) {
            delete msg;
            throw client_error(20, "Error 20 - unexpcted message");
        }

        FileChunkMsg *fcmsg = dynamic_cast<FileChunkMsg*>(msg);
        compressed += fcmsg->compressed;
        uncompressed += fcmsg->len;

        if (write(obj_fd, fcmsg->buffer, fcmsg->len) != (ssize_t)fcmsg->len) {
            delete msg;
            throw client_error(21, "Error 21 - error writing file");
        }
    }

    if (uncompressed)
        trace() << "got " << compressed << " bytes ("
                << (compressed * 100 / uncompressed) << "%)" << endl;

    delete msg;
}

static void receive_file(const string& output_file, MsgChannel* cserver)
{
    string tmp_file = output_file + "_icetmp";
    int obj_fd = open(tmp_file.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_LARGEFILE, 0666);
    if (obj_fd == -1) {
        std::string errmsg("can't create ");
        errmsg += tmp_file + ":";
        log_perror(errmsg.c_str());
        throw(31);
    }

    try {
        receive_fd(obj_fd, cserver);
    } catch (...) {
        unlink(tmp_file.c_str());
        throw;
    }

    if (close(obj_fd) != 0 || rename(tmp_file.c_str(), output_file.c_str()) != 0) {
        unlink(tmp_file.c_str());
        throw client_error(30, "Error 30 - error closing temp file");
    }
}

static int build_remote_int(CompileJob &job, UseCSMsg *usecs, MsgChannel *local_daemon,
                            const string &environment, const string &version_file,
                            const char *preproc_file, bool output)
{
    string hostname = usecs->hostname;
    unsigned int port = usecs->port;
    int job_id = usecs->job_id;
    bool got_env = usecs->got_env;
    job.setJobID(job_id);
    job.setEnvironmentVersion(environment);   // hoping on the scheduler's wisdom
    trace() << "Have to use host " << hostname << ":" << port << " - Job ID: "
            << job.jobID() << " - env: " << usecs->host_platform
            << " - has env: " << (got_env ? "true" : "false")
            << " - match j: " << usecs->matched_job_id
            << "\n";

    int status = 255;

    MsgChannel *cserver = 0;

    try {
        cserver = Service::createChannel(hostname, port, 10);

        if (!cserver) {
            log_error() << "no server found behind given hostname " << hostname << ":"
                        << port << endl;
            throw client_error(2, "Error 2 - no server found at " + hostname);
        }

        if (!got_env) {
            log_block b("Transfer Environment");
            // transfer env
            struct stat buf;

            if (stat(version_file.c_str(), &buf)) {
                log_perror("error stat'ing version file");
                throw client_error(4, "Error 4 - unable to stat version file");
            }

            EnvTransferMsg msg(job.targetPlatform(), job.environmentVersion());

            if (!cserver->send_msg(msg)) {
                throw client_error(6, "Error 6 - send environment to remove failed");
            }

            int env_fd = open(version_file.c_str(), O_RDONLY);

            if (env_fd < 0) {
                throw client_error(5, "Error 5 - unable to open version file:\n\t" + version_file);
            }

            write_server_cpp(env_fd, cserver);

            if (!cserver->send_msg(EndMsg())) {
                log_error() << "write of environment failed" << endl;
                throw client_error(8, "Error 8 - write environment to remote failed");
            }

            if (IS_PROTOCOL_31(cserver)) {
                VerifyEnvMsg verifymsg(job.targetPlatform(), job.environmentVersion());

                if (!cserver->send_msg(verifymsg)) {
                    throw client_error(22, "Error 22 - error sending environment");
                }

                Msg *verify_msg = cserver->get_msg(60);

                if (verify_msg && verify_msg->type == M_VERIFY_ENV_RESULT) {
                    if (!static_cast<VerifyEnvResultMsg*>(verify_msg)->ok) {
                        // The remote can't handle the environment at all (e.g. kernel too old),
                        // mark it as never to be used again for this environment.
                        log_info() << "Host " << hostname
                                   << " did not successfully verify environment."
                                   << endl;
                        BlacklistHostEnvMsg blacklist(job.targetPlatform(),
                                                      job.environmentVersion(), hostname);
                        local_daemon->send_msg(blacklist);
                        throw client_error(24, "Error 24 - remote " + hostname + " unable to handle environment");
                    } else
                        trace() << "Verified host " << hostname << " for environment "
                                << job.environmentVersion() << " (" << job.targetPlatform() << ")"
                                << endl;
                } else {
                    throw client_error(25, "Error 25 - other error verifying environment on remote");
                }
            }
        }

        if (!IS_PROTOCOL_31(cserver) && ignore_unverified()) {
            log_warning() << "Host " << hostname << " cannot be verified." << endl;
            throw client_error(26, "Error 26 - environment on " + hostname + " cannot be verified");
        }

        CompileFileMsg compile_file(&job);
        {
            log_block b("send compile_file");

            if (!cserver->send_msg(compile_file)) {
                log_info() << "write of job failed" << endl;
                throw client_error(9, "Error 9 - error sending file to remote");
            }
        }

        if (job.streaming()) {
            log_block bl2("sending stdin");
            write_server_cpp(STDIN_FILENO, cserver);
        } else if (!preproc_file) {
            int sockets[2];

            if (pipe(sockets)) {
                /* for all possible cases, this is something severe */
                exit(errno);
            }

            /* This will fork, and return the pid of the child.  It will not
               return for the child itself.  If it returns normally it will have
               closed the write fd, i.e. sockets[1].  */
            pid_t cpp_pid = call_cpp(job, sockets[1], sockets[0]);

            if (cpp_pid == -1) {
                throw client_error(18, "Error 18 - (fork error?)");
            }

            try {
                log_block bl2("write_server_cpp from cpp");
                write_server_cpp(sockets[0], cserver);
            } catch (...) {
                kill(cpp_pid, SIGTERM);
                throw;
            }

            log_block wait_cpp("wait for cpp");

            while (waitpid(cpp_pid, &status, 0) < 0 && errno == EINTR) {}

            if (shell_exit_status(status) != 0) {   // failure
                delete cserver;
                cserver = 0;
                return shell_exit_status(status);
            }
        } else {
            int cpp_fd = open(preproc_file, O_RDONLY);

            if (cpp_fd < 0) {
                throw client_error(11, "Error 11 - unable to open preprocessed file");
            }

            log_block cpp_block("write_server_cpp");
            write_server_cpp(cpp_fd, cserver);
        }

        if (!cserver->send_msg(EndMsg())) {
            log_info() << "write of end failed" << endl;
            throw client_error(12, "Error 12 - failed to send file to remote");
        }

        Msg *msg;
        {
            log_block wait_cs("wait for cs");
            msg = cserver->get_msg(12 * 60);

            if (!msg) {
                throw client_error(14, "Error 14 - error reading message from remote");
            }
        }

        check_for_failure(msg, cserver);

        if (msg->type != M_COMPILE_RESULT) {
            log_warning() << "waited for compile result, but got " << (char)msg->type << endl;
            delete msg;
            throw client_error(13, "Error 13 - did not get compile response message");
        }

        CompileResultMsg *crmsg = dynamic_cast<CompileResultMsg*>(msg);
        assert(crmsg);

        status = crmsg->status;

        if (status && crmsg->was_out_of_memory) {
            delete crmsg;
            log_info() << "the server ran out of memory, recompiling locally" << endl;
            throw remote_error(101, "Error 101 - the server ran out of memory, recompiling locally");
        }

        if (output) {
            if ((!crmsg->out.empty() || !crmsg->err.empty()) && output_needs_workaround(job)) {
                delete crmsg;
                log_info() << "command needs stdout/stderr workaround, recompiling locally" << endl;
                throw remote_error(102, "Error 102 - command needs stdout/stderr workaround, recompiling locally");
            }

            ignore_result(write(STDOUT_FILENO, crmsg->out.c_str(), crmsg->out.size()));

            if (colorify_wanted(job)) {
                colorify_output(crmsg->err);
            } else {
                ignore_result(write(STDERR_FILENO, crmsg->err.c_str(), crmsg->err.size()));
            }

            if (status && (crmsg->err.length() || crmsg->out.length())) {
                log_error() << "Compiled on " << hostname << endl;
            }
        }

        bool have_dwo_file = crmsg->have_dwo_file;
        delete crmsg;

        if (status == 0) {
            if (job.streaming()) {
                receive_fd(STDOUT_FILENO, cserver);
            } else {
                assert(!job.outputFile().empty());
                receive_file(job.outputFile(), cserver);
                if (have_dwo_file) {
                    string dwo_output = job.outputFile().substr(0, job.outputFile().find_last_of('.')) + ".dwo";
                    receive_file(dwo_output, cserver);
                }
            }
        }

    } catch (...) {
        // Handle pending status messages, if any.
        if(cserver) {
            while(Msg* msg = cserver->get_msg(0)) {
                if(msg->type == M_STATUS_TEXT)
                    log_error() << "Remote status (compiled on " << cserver->name << "): "
                                << static_cast<StatusTextMsg*>(msg)->text << endl;
                delete msg;
            }
            delete cserver;
            cserver = 0;
        }

        throw;
    }

    delete cserver;
    return status;
}

static string
md5_for_file(const string & file)
{
    md5_state_t state;
    string result;

    md5_init(&state);
    FILE *f = fopen(file.c_str(), "rb");

    if (!f) {
        return result;
    }

    md5_byte_t buffer[40000];

    while (true) {
        size_t size = fread(buffer, 1, 40000, f);

        if (!size) {
            break;
        }

        md5_append(&state, buffer, size);
    }

    fclose(f);

    md5_byte_t digest[16];
    md5_finish(&state, digest);

    char digest_cache[33];

    for (int di = 0; di < 16; ++di) {
        sprintf(digest_cache + di * 2, "%02x", digest[di]);
    }

    digest_cache[32] = 0;
    result = digest_cache;
    return result;
}

static bool
maybe_build_local(MsgChannel *local_daemon, UseCSMsg *usecs, CompileJob &job,
                  int &ret)
{
    remote_daemon = usecs->hostname;

    if (usecs->hostname == "127.0.0.1") {
        // If this is a test build, do local builds on the local daemon
        // that has --no-remote, use remote building for the remaining ones.
        if (getenv("ICECC_TEST_REMOTEBUILD") && usecs->port != 0 )
            return false;
        trace() << "building myself, but telling localhost\n";
        int job_id = usecs->job_id;
        job.setJobID(job_id);
        job.setEnvironmentVersion("__client");
        CompileFileMsg compile_file(&job);

        if (!local_daemon->send_msg(compile_file)) {
            log_info() << "write of job failed" << endl;
            throw client_error(29, "Error 29 - write of job failed");
        }

        struct timeval begintv,  endtv;

        struct rusage ru;

        gettimeofday(&begintv, 0);

        ret = build_local(job, local_daemon, &ru);

        gettimeofday(&endtv, 0);

        // filling the stats, so the daemon can play proxy for us
        JobDoneMsg msg(job_id, ret, JobDoneMsg::FROM_SUBMITTER);

        msg.real_msec = (endtv.tv_sec - begintv.tv_sec) * 1000 + (endtv.tv_usec - begintv.tv_usec) / 1000;

        struct stat st;

        msg.out_uncompressed = 0;
        if (!stat(job.outputFile().c_str(), &st)) {
            msg.out_uncompressed += st.st_size;
        }
        if (!stat((job.outputFile().substr(0, job.outputFile().find_last_of('.')) + ".dwo").c_str(), &st)) {
            msg.out_uncompressed += st.st_size;
        }

        msg.user_msec = ru.ru_utime.tv_sec * 1000 + ru.ru_utime.tv_usec / 1000;
        msg.sys_msec = ru.ru_stime.tv_sec * 1000 + ru.ru_stime.tv_usec / 1000;
        msg.pfaults = ru.ru_majflt + ru.ru_minflt + ru.ru_nswap;
        msg.exitcode = ret;

        if (msg.user_msec > 50 && msg.out_uncompressed > 1024) {
            trace() << "speed=" << float(msg.out_uncompressed / msg.user_msec) << endl;
        }

        return local_daemon->send_msg(msg);
    }

    return false;
}

// Minimal version of remote host that we want to use for the job.
static int minimalRemoteVersion( const CompileJob& )
{
    int version = MIN_PROTOCOL_VERSION;
    if( ignore_unverified())
        version = max( version, 31 );
    return version;
}

int build_remote(CompileJob &job, MsgChannel *local_daemon, const Environments &_envs, int permill)
{
    srand(time(0) + getpid());

    int torepeat = 1;
    bool has_split_dwarf = job.dwarfFissionEnabled();

    // older compilers do not support the options we need to make it reproducible
#if defined(__GNUC__) && ( ( (__GNUC__ == 3) && (__GNUC_MINOR__ >= 3) ) || (__GNUC__ >=4) )

    if (!compiler_is_clang(job) && !job.streaming()) {
        if (rand() % 1000 < permill) {
            torepeat = 3;
        }
    }

#endif

    trace() << job.inputFile() << " compiled " << torepeat << " times on "
            << job.targetPlatform() << "\n";

    map<string, string> versionfile_map, version_map;
    Environments envs = rip_out_paths(_envs, version_map, versionfile_map);

    if (!envs.size()) {
        log_error() << "$ICECC_VERSION needs to point to .tar files" << endl;
        throw client_error(22, "Error 22 - $ICECC_VERSION needs to point to .tar files");
    }

    const char *preferred_host = getenv("ICECC_PREFERRED_HOST");

    if (torepeat == 1) {
        string fake_filename;
        list<string> args = job.remoteFlags();

        for (list<string>::const_iterator it = args.begin(); it != args.end(); ++it) {
            fake_filename += "/" + *it;
        }

        args = job.restFlags();

        for (list<string>::const_iterator it = args.begin(); it != args.end(); ++it) {
            fake_filename += "/" + *it;
        }

        fake_filename += get_absfilename(job.inputFile());

        GetCSMsg getcs(envs, fake_filename, job.language(), torepeat,
                       job.targetPlatform(), job.argumentFlags(),
                       preferred_host ? preferred_host : string(),
                       minimalRemoteVersion(job));

        if (!local_daemon->send_msg(getcs)) {
            log_warning() << "asked for CS" << endl;
            throw client_error(24, "Error 24 - asked for CS");
        }

        UseCSMsg *usecs = get_server(local_daemon);
        int ret;

        if (!maybe_build_local(local_daemon, usecs, job, ret))
            ret = build_remote_int(job, usecs, local_daemon,
                                   version_map[usecs->host_platform],
                                   versionfile_map[usecs->host_platform],
                                   0, true);

        delete usecs;
        return ret;
    } else {
        char *preproc = 0;
        dcc_make_tmpnam("icecc", ".ix", &preproc, 0);
        const CharBufferDeleter preproc_holder(preproc);
        int cpp_fd = open(preproc, O_WRONLY);
        /* When call_cpp returns normally (for the parent) it will have closed
           the write fd, i.e. cpp_fd.  */
        pid_t cpp_pid = call_cpp(job, cpp_fd);

        if (cpp_pid == -1) {
            ::unlink(preproc);
            throw client_error(10, "Error 10 - (unable to fork process?)");
        }

        int status = 255;
        waitpid(cpp_pid, &status, 0);

        if (shell_exit_status(status)) {   // failure
            ::unlink(preproc);
            return shell_exit_status(status);
        }

        char rand_seed[400]; // "designed to be oversized" (Levi's)
        sprintf(rand_seed, "-frandom-seed=%d", rand());
        job.appendFlag(rand_seed, Arg_Remote);

        GetCSMsg getcs(envs, get_absfilename(job.inputFile()), job.language(), torepeat,
                       job.targetPlatform(), job.argumentFlags(),
                       preferred_host ? preferred_host : string(),
                       minimalRemoteVersion(job));


        if (!local_daemon->send_msg(getcs)) {
            log_warning() << "asked for CS" << endl;
            throw client_error(0, "Error 0 - asked for CS");
        }

        map<pid_t, int> jobmap;
        CompileJob *jobs = new CompileJob[torepeat];
        UseCSMsg **umsgs = new UseCSMsg*[torepeat];

        bool misc_error = false;
        int *exit_codes = new int[torepeat];

        for (int i = 0; i < torepeat; i++) { // init
            exit_codes[i] = 42;
        }


        for (int i = 0; i < torepeat; i++) {
            jobs[i] = job;
            char *buffer = 0;

            if (i) {
                dcc_make_tmpnam("icecc", ".o", &buffer, 0);
                jobs[i].setOutputFile(buffer);
            } else {
                buffer = strdup(job.outputFile().c_str());
            }

            const CharBufferDeleter buffer_holder(buffer);

            umsgs[i] = get_server(local_daemon);

            remote_daemon = umsgs[i]->hostname;

            trace() << "got_server_for_job " << umsgs[i]->hostname << endl;

            flush_debug();

            pid_t pid = fork();

            if (!pid) {
                int ret = 42;

                try {
                    if (!maybe_build_local(local_daemon, umsgs[i], jobs[i], ret))
                        ret = build_remote_int(
                                  jobs[i], umsgs[i], local_daemon,
                                  version_map[umsgs[i]->host_platform],
                                  versionfile_map[umsgs[i]->host_platform],
                                  preproc, i == 0);
                } catch (std::exception& error) {
                    log_info() << "build_remote_int failed and has thrown " << error.what() << endl;
                    kill(getpid(), SIGTERM);
                    return 0; // shouldn't matter
                }

                _exit(ret);
                return 0; // doesn't matter
            }

            jobmap[pid] = i;
        }

        for (int i = 0; i < torepeat; i++) {
            pid_t pid = wait(&status);

            if (pid < 0) {
                log_perror("wait failed");
                status = -1;
            } else {
                if (WIFSIGNALED(status)) {
                    // there was some misc error in processing
                    misc_error = true;
                    break;
                }

                exit_codes[jobmap[pid]] = shell_exit_status(status);
            }
        }

        if (!misc_error) {
            string first_md5 = md5_for_file(jobs[0].outputFile());

            for (int i = 1; i < torepeat; i++) {
                if (!exit_codes[0]) {   // if the first failed, we fail anyway
                    if (exit_codes[i] == 42) { // they are free to fail for misc reasons
                        continue;
                    }

                    if (exit_codes[i]) {
                        log_error() << umsgs[i]->hostname << " compiled with exit code " << exit_codes[i]
                                    << " and " << umsgs[0]->hostname << " compiled with exit code "
                                    << exit_codes[0] << " - aborting!\n";
                        ::unlink(jobs[0].outputFile().c_str());
                        if (has_split_dwarf) {
                            string dwo_file = jobs[0].outputFile().substr(0, jobs[0].outputFile().find_last_of('.')) + ".dwo";
                            ::unlink(dwo_file.c_str());
                        }
                        exit_codes[0] = -1; // overwrite
                        break;
                    }

                    string other_md5 = md5_for_file(jobs[i].outputFile());

                    if (other_md5 != first_md5) {
                        log_error() << umsgs[i]->hostname << " compiled "
                                    << jobs[0].outputFile() << " with md5 sum " << other_md5
                                    << "(" << jobs[i].outputFile() << ")" << " and "
                                    << umsgs[0]->hostname << " compiled with md5 sum "
                                    << first_md5 << " - aborting!\n";
                        rename(jobs[0].outputFile().c_str(),
                               (jobs[0].outputFile() + ".caught").c_str());
                        rename(preproc, (string(preproc) + ".caught").c_str());
                        if (has_split_dwarf) {
                            string dwo_file = jobs[0].outputFile().substr(0, jobs[0].outputFile().find_last_of('.')) + ".dwo";
                            rename(dwo_file.c_str(), (dwo_file + ".caught").c_str());
                        }
                        exit_codes[0] = -1; // overwrite
                        break;
                    }
                }

                ::unlink(jobs[i].outputFile().c_str());
                if (has_split_dwarf) {
                    string dwo_file = jobs[i].outputFile().substr(0, jobs[i].outputFile().find_last_of('.')) + ".dwo";
                    ::unlink(dwo_file.c_str());
                }
                delete umsgs[i];
            }
        } else {
            ::unlink(jobs[0].outputFile().c_str());
            if (has_split_dwarf) {
                string dwo_file = jobs[0].outputFile().substr(0, jobs[0].outputFile().find_last_of('.')) + ".dwo";
                ::unlink(dwo_file.c_str());
            }

            for (int i = 1; i < torepeat; i++) {
                ::unlink(jobs[i].outputFile().c_str());
                if (has_split_dwarf) {
                    string dwo_file = jobs[i].outputFile().substr(0, jobs[i].outputFile().find_last_of('.')) + ".dwo";
                    ::unlink(dwo_file.c_str());
                }
                delete umsgs[i];
            }
        }

        delete umsgs[0];

        ::unlink(preproc);

        int ret = exit_codes[0];

        delete [] umsgs;
        delete [] jobs;
        delete [] exit_codes;

        if (misc_error) {
            throw client_error(27, "Error 27 - misc error");
        }

        return ret;
    }


    return 0;
}
