/*
 * This file is part of MPlayer.
 *
 * Original authors: Albeu, probably Arpi
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "osdep/io.h"

#include "common/msg.h"
#include "stream.h"
#include "options/m_option.h"
#include "options/path.h"

#if HAVE_BSD_FSTATFS
#include <sys/param.h>
#include <sys/mount.h>
#endif

#if HAVE_LINUX_FSTATFS
#include <sys/vfs.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <winternl.h>
#include <io.h>

#ifndef FILE_REMOTE_DEVICE
#define FILE_REMOTE_DEVICE (0x10)
#endif
#endif

struct priv {
    int fd;
    bool close;
};

static int fill_buffer(stream_t *s, char *buffer, int max_len)
{
    struct priv *p = s->priv;
    int r = read(p->fd, buffer, max_len);
    return (r <= 0) ? -1 : r;
}

static int write_buffer(stream_t *s, char *buffer, int len)
{
    struct priv *p = s->priv;
    int r;
    int wr = 0;
    while (wr < len) {
        r = write(p->fd, buffer, len);
        if (r <= 0)
            return -1;
        wr += r;
        buffer += r;
    }
    return len;
}

static int seek(stream_t *s, int64_t newpos)
{
    struct priv *p = s->priv;
    return lseek(p->fd, newpos, SEEK_SET) != (off_t)-1;
}

static int control(stream_t *s, int cmd, void *arg)
{
    struct priv *p = s->priv;
    switch (cmd) {
    case STREAM_CTRL_GET_SIZE: {
        off_t size = lseek(p->fd, 0, SEEK_END);
        lseek(p->fd, s->pos, SEEK_SET);
        if (size != (off_t)-1) {
            *(int64_t *)arg = size;
            return 1;
        }
        break;
    }
    }
    return STREAM_UNSUPPORTED;
}

static void s_close(stream_t *s)
{
    struct priv *p = s->priv;
    if (p->close && p->fd >= 0)
        close(p->fd);
}

// If url is a file:// URL, return the local filename, otherwise return NULL.
char *mp_file_url_to_filename(void *talloc_ctx, bstr url)
{
    bstr proto = mp_split_proto(url, &url);
    if (bstrcasecmp0(proto, "file") != 0)
        return NULL;
    char *filename = bstrto0(talloc_ctx, url);
    mp_url_unescape_inplace(filename);
#if HAVE_DOS_PATHS
    // extract '/' from '/x:/path'
    if (filename[0] == '/' && filename[1] && filename[2] == ':')
        memmove(filename, filename + 1, strlen(filename)); // including \0
#endif
    return filename;
}

#if HAVE_BSD_FSTATFS
static bool check_stream_network(stream_t *stream)
{
    struct statfs fs;
    const char *stypes[] = { "afpfs", "nfs", "smbfs", "webdav", NULL };
    struct priv *priv = stream->priv;
    if (fstatfs(priv->fd, &fs) == 0)
        for (int i=0; stypes[i]; i++)
            if (strcmp(stypes[i], fs.f_fstypename) == 0)
                return true;
    return false;

}
#elif HAVE_LINUX_FSTATFS
static bool check_stream_network(stream_t *stream)
{
    struct statfs fs;
    const uint32_t stypes[] = {
        0x5346414F  /*AFS*/,    0x61756673  /*AUFS*/,   0x00C36400  /*CEPH*/,
        0xFF534D42  /*CIFS*/,   0x73757245  /*CODA*/,   0x19830326  /*FHGFS*/,
        0x65735546  /*FUSEBLK*/,0x65735543  /*FUSECTL*/,0x1161970   /*GFS*/,
        0x47504653  /*GPFS*/,   0x6B414653  /*KAFS*/,   0x0BD00BD0  /*LUSTRE*/,
        0x564C      /*NCP*/,    0x6969      /*NFS*/,    0x6E667364  /*NFSD*/,
        0xAAD7AAEA  /*PANFS*/,  0x50495045  /*PIPEFS*/, 0x517B      /*SMB*/,
        0xBEEFDEAD  /*SNFS*/,   0xBACBACBC  /*VMHGFS*/, 0x7461636f  /*OCFS2*/,
        0
    };
    struct priv *priv = stream->priv;
    if (fstatfs(priv->fd, &fs) == 0) {
        for (int i=0; stypes[i]; i++) {
            if (stypes[i] == fs.f_type)
                return true;
        }
    }
    return false;

}
#elif defined(_WIN32)
static bool check_stream_network(stream_t *stream)
{
    NTSTATUS (NTAPI *pNtQueryVolumeInformationFile)(HANDLE,
        PIO_STATUS_BLOCK, PVOID, ULONG, FS_INFORMATION_CLASS) = NULL;

    // NtQueryVolumeInformationFile is an internal Windows function. It has
    // been present since Windows XP, however this code should fail gracefully
    // if it's removed from a future version of Windows.
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    pNtQueryVolumeInformationFile = (NTSTATUS (NTAPI*)(HANDLE,
        PIO_STATUS_BLOCK, PVOID, ULONG, FS_INFORMATION_CLASS))
        GetProcAddress(ntdll, "NtQueryVolumeInformationFile");

    if (!pNtQueryVolumeInformationFile)
        return false;

    struct priv *priv = stream->priv;
    HANDLE h = (HANDLE)_get_osfhandle(priv->fd);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    FILE_FS_DEVICE_INFORMATION info = { 0 };
    IO_STATUS_BLOCK io;
    NTSTATUS status = pNtQueryVolumeInformationFile(h, &io, &info,
        sizeof(info), FileFsDeviceInformation);
    if (!NT_SUCCESS(status))
        return false;

    return info.DeviceType == FILE_DEVICE_NETWORK_FILE_SYSTEM ||
           (info.Characteristics & FILE_REMOTE_DEVICE);
}
#else
static bool check_stream_network(stream_t *stream)
{
    return false;
}
#endif

static int open_f(stream_t *stream)
{
    int fd;
    struct priv *priv = talloc_ptrtype(stream, priv);
    *priv = (struct priv) {
        .fd = -1
    };
    stream->priv = priv;

    bool write = stream->mode == STREAM_WRITE;
    int m = O_CLOEXEC | (write ? O_RDWR | O_CREAT | O_TRUNC : O_RDONLY);

    char *filename = mp_file_url_to_filename(stream, bstr0(stream->url));
    if (filename) {
        stream->path = filename;
    } else {
        filename = stream->path;
    }

    if (!strcmp(filename, "-")) {
        if (!write) {
            MP_INFO(stream, "Reading from stdin...\n");
            fd = 0;
        } else {
            MP_INFO(stream, "Writing to stdout...\n");
            fd = 1;
        }
#ifdef __MINGW32__
        setmode(fd, O_BINARY);
#endif
        priv->fd = fd;
        priv->close = false;
    } else {
        mode_t openmode = S_IRUSR | S_IWUSR;
#ifndef __MINGW32__
        openmode |= S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
#endif
        fd = open(filename, m | O_BINARY, openmode);
        if (fd < 0) {
            MP_ERR(stream, "Cannot open file '%s': %s\n",
                    filename, strerror(errno));
            return STREAM_ERROR;
        }
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
            MP_ERR(stream, "File is a directory: '%s'\n", filename);
            close(fd);
            return STREAM_ERROR;
        }
        priv->fd = fd;
        priv->close = true;
    }

    off_t len = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (len != (off_t)-1) {
        stream->seek = seek;
        stream->seekable = true;
    }

    stream->type = STREAMTYPE_FILE;
    stream->fast_skip = true;
    stream->fill_buffer = fill_buffer;
    stream->write_buffer = write_buffer;
    stream->control = control;
    stream->read_chunk = 64 * 1024;
    stream->close = s_close;

    if (check_stream_network(stream))
        stream->streaming = true;

    return STREAM_OK;
}

const stream_info_t stream_info_file = {
    .name = "file",
    .open = open_f,
    .protocols = (const char*[]){ "file", "", NULL },
    .can_write = true,
};
