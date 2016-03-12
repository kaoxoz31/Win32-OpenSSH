/*
* Author: Manoj Ampalam <manoj.ampalam@microsoft.com>
*/

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <io.h>
#include "w32fd.h"
#include "inc/defs.h"
#include <errno.h>
#include <stddef.h>

/* internal read buffer size */
#define READ_BUFFER_SIZE 100*1024
/* internal write buffer size */
#define WRITE_BUFFER_SIZE 100*1024
#define errno_from_Win32LastError() errno_from_Win32Error(GetLastError())

/* maps Win32 error to errno */
static int 
errno_from_Win32Error(int win32_error)
{
	switch (win32_error) {
	case ERROR_ACCESS_DENIED:
		return EACCES;
	case ERROR_OUTOFMEMORY:
		return ENOMEM;
	case ERROR_FILE_EXISTS:
		return EEXIST;
	case ERROR_FILE_NOT_FOUND:
		return ENOENT;
	default:
		return EOTHER;
	}
}

/* used to name named pipes used to implement pipe() */
static int pipe_counter = 0;

/*
 * pipe() implementation. Creates an inbound named pipe, uses CreateFile to connect
 * to it. These handles are associated with read end and write end of the pipe
 */
int 
fileio_pipe(struct w32_io* pio[2]) {
	HANDLE read_handle = INVALID_HANDLE_VALUE, write_handle = INVALID_HANDLE_VALUE;
	struct w32_io *pio_read = NULL, *pio_write = NULL;
	char pipe_name[MAX_PATH];
	SECURITY_ATTRIBUTES sec_attributes;

	if (pio == NULL) {
		errno = EINVAL;
		debug("pipe - ERROR invalid parameter");
		return -1;
	}

	/* create name for named pipe */
	if (-1 == sprintf_s(pipe_name, MAX_PATH, "\\\\.\\Pipe\\W32PosixPipe.%08x.%08x", 
		GetCurrentProcessId(), pipe_counter++)) {
		errno = EOTHER;
		debug("pipe - ERROR sprintf_s %d", errno);
		goto error;
	}

	sec_attributes.bInheritHandle = TRUE;
	sec_attributes.lpSecurityDescriptor = NULL;
	sec_attributes.nLength = 0;

	/* create named pipe */
	read_handle = CreateNamedPipeA(pipe_name,
		PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
		PIPE_TYPE_BYTE | PIPE_WAIT,
		1,
		4096,
		4096,
		0,
		&sec_attributes);
	if (read_handle == INVALID_HANDLE_VALUE) {
		errno = errno_from_Win32LastError();
		debug("pipe - CreateNamedPipe() ERROR:%d", errno);
		goto error;
	}

	/* connect to named pipe */
	write_handle = CreateFileA(pipe_name,
		GENERIC_WRITE,
		0,
		&sec_attributes,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
		NULL);
	if (write_handle == INVALID_HANDLE_VALUE) {
		errno = errno_from_Win32LastError();
		debug("pipe - ERROR CreateFile() :%d", errno);
		goto error;
	}

	/* create w32_io objects encapsulating above handles */
	pio_read = (struct w32_io*)malloc(sizeof(struct w32_io));
	pio_write = (struct w32_io*)malloc(sizeof(struct w32_io));

	if (!pio_read || !pio_write) {
		errno = ENOMEM;
		debug("pip - ERROR:%d", errno);
		goto error;
	}

	memset(pio_read, 0, sizeof(struct w32_io));
	memset(pio_write, 0, sizeof(struct w32_io));

	pio_read->handle = read_handle;
	pio_read->internal.state = PIPE_READ_END;
	pio_write->handle = write_handle;
	pio_write->internal.state = PIPE_WRITE_END;

	pio[0] = pio_read;
	pio[1] = pio_write;
	return 0;

error:
	if (read_handle)
		CloseHandle(read_handle);
	if (write_handle)
		CloseHandle(write_handle);
	if (pio_read)
		free(pio_read);
	if (pio_write)
		free(pio_write);
	return -1;
}

struct createFile_flags {
	DWORD dwDesiredAccess;
	DWORD dwShareMode;
	SECURITY_ATTRIBUTES securityAttributes;
	DWORD dwCreationDisposition;
	DWORD dwFlagsAndAttributes;
};

/* maps open() file modes and flags to ones needed by CreateFile */
static int 
createFile_flags_setup(int flags, int mode, struct createFile_flags* cf_flags) {

	/* check flags */
	int rwflags = flags & 0xf;
	int c_s_flags = flags & 0xfffffff0;

	/* 
	* should be one of one of the following access modes: 
	* O_RDONLY, O_WRONLY, or O_RDWR
	*/
	if ((rwflags != O_RDONLY) && (rwflags != O_WRONLY) && (rwflags != O_RDWR)) {
		debug("open - flags ERROR: wrong rw flags: %d", flags);
		errno = EINVAL;
		return -1;
	}

	/*only following create and status flags currently supported*/
	if (c_s_flags & ~(O_NONBLOCK | O_APPEND | O_CREAT | O_TRUNC
	    | O_EXCL | O_BINARY)) {
		debug("open - ERROR: Unsupported flags: %d", flags);
		errno = ENOTSUP;
		return -1;
	}

	/*validate mode*/
	if (mode &~(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) {
		debug("open - ERROR: unsupported mode: %d", mode);
		errno = ENOTSUP;
		return -1;
	}

	switch (rwflags) {
	case O_RDONLY:
		cf_flags->dwDesiredAccess = GENERIC_READ;
		break;
	case O_WRONLY:
		cf_flags->dwDesiredAccess = GENERIC_WRITE;
		break;
	case O_RDWR:
		cf_flags->dwDesiredAccess = GENERIC_READ | GENERIC_WRITE;
		break;
	}

	cf_flags->dwShareMode = 0;

	cf_flags->securityAttributes.lpSecurityDescriptor = NULL;
	cf_flags->securityAttributes.bInheritHandle = TRUE;
	cf_flags->securityAttributes.nLength = 0;

	cf_flags->dwCreationDisposition = OPEN_EXISTING;
	if (c_s_flags & O_TRUNC)
		cf_flags->dwCreationDisposition = TRUNCATE_EXISTING;
	if (c_s_flags & O_CREAT) {
		if (c_s_flags & O_EXCL)
			cf_flags->dwCreationDisposition = CREATE_NEW;
		else
			cf_flags->dwCreationDisposition = OPEN_ALWAYS;
	}

	if (c_s_flags & O_APPEND)
		cf_flags->dwDesiredAccess = FILE_APPEND_DATA;

	cf_flags->dwFlagsAndAttributes = FILE_FLAG_OVERLAPPED | SECURITY_IMPERSONATION;

	/*TODO - map mode */

	return 0;
}

/* open() implementation. Uses CreateFile to open file, console, device, etc */
struct w32_io* 
fileio_open(const char *pathname, int flags, int mode) {
	struct w32_io* pio = NULL;
	struct createFile_flags cf_flags;
	HANDLE handle;

	debug2("open - pathname:%s, flags:%d, mode:%d", pathname, flags, mode);
	/* check input params*/
	if (pathname == NULL) {
		errno = EINVAL;
		debug("open - ERROR:%d", errno);
		return NULL;
	}


	if (createFile_flags_setup(flags, mode, &cf_flags) == -1)
		return NULL;

	/* TODO - Use unicode version.*/
	handle = CreateFileA(pathname, cf_flags.dwDesiredAccess, cf_flags.dwShareMode, 
	    &cf_flags.securityAttributes, cf_flags.dwCreationDisposition, 
	    cf_flags.dwFlagsAndAttributes, NULL);

	if (handle == INVALID_HANDLE_VALUE) {
		errno = errno_from_Win32LastError();
		debug("open - CreateFile ERROR:%d", GetLastError());
		return NULL;
	}

	pio = (struct w32_io*)malloc(sizeof(struct w32_io));
	if (pio == NULL) {
		CloseHandle(handle);
		errno = ENOMEM;
		debug("open - ERROR:%d", errno);
		return NULL;
	}
	memset(pio, 0, sizeof(struct w32_io));

	if (flags & O_NONBLOCK)
		pio->fd_status_flags = O_NONBLOCK;

	pio->handle = handle;
	return pio;
}


VOID CALLBACK ReadCompletionRoutine(
    _In_    DWORD        dwErrorCode,
    _In_    DWORD        dwNumberOfBytesTransfered,
    _Inout_ LPOVERLAPPED lpOverlapped
	) {
	struct w32_io* pio = 
	    (struct w32_io*)((char*)lpOverlapped - offsetof(struct w32_io, read_overlapped));
	debug2("ReadCB pio:%p, pending_state:%d, error:%d, received:%d", 
		pio, pio->read_details.pending, dwErrorCode, dwNumberOfBytesTransfered);
	pio->read_details.error = dwErrorCode;
	pio->read_details.remaining = dwNumberOfBytesTransfered;
	pio->read_details.completed = 0;
	pio->read_details.pending = FALSE;
	*((__int64*)&lpOverlapped->Offset) += dwNumberOfBytesTransfered;
}

/* initiate an async read */
int 
fileio_ReadFileEx(struct w32_io* pio) {
	HANDLE h = pio->handle;
	debug2("ReadFileEx io:%p", pio);
	if (pio->read_details.buf == NULL){
		pio->read_details.buf = malloc(READ_BUFFER_SIZE);
		if (!pio->read_details.buf) {
			errno = ENOMEM;
			debug2("ReadFileEx - ERROR: %d, io:%p", errno, pio);
			return -1;
		}
		pio->read_details.buf_size = READ_BUFFER_SIZE;
	}

	/* get underlying handle for standard io */
	if (pio->type == STD_IO_FD)
		h = GetStdHandle(pio->std_handle);
	
	if (ReadFileEx(h, pio->read_details.buf, pio->read_details.buf_size, 
	    &pio->read_overlapped, &ReadCompletionRoutine)) 
		pio->read_details.pending = TRUE;
	else {
		errno = errno_from_Win32LastError();
		debug("ReadFileEx() ERROR:%d, io:%p", GetLastError(), pio);
		return -1;
	}

	return 0;
}

/* read() implementation */
int 
fileio_read(struct w32_io* pio, void *dst, unsigned int max) {
	int bytes_copied;

	debug3("read - io:%p remaining:%d", pio, pio->read_details.remaining);
	if ((pio->type == PIPE_FD) && (pio->internal.state == PIPE_WRITE_END)) {
		debug("read - ERROR: called on write end of pipe, io:%p", pio);
		errno = EBADF;
		return -1;
	}

	/* if read is pending */
	if (pio->read_details.pending) {
		if (w32_io_is_blocking(pio)) {
			debug2("read - io is pending, blocking call made, io:%p", pio);
			while (fileio_is_io_available(pio, TRUE) == FALSE) {
				if (-1 == wait_for_any_event(NULL, 0, INFINITE))
					return -1;
			}
		}
		errno = EAGAIN;
		debug2("read - io is already pending, io:%p", pio);
		return -1;
	}

	if (fileio_is_io_available(pio, TRUE) == FALSE) {
		if (-1 == fileio_ReadFileEx(pio)) {
			if ((pio->type == PIPE_FD) && (errno == ERROR_NEGATIVE_SEEK)) {
				/* write end of the pipe closed */
				debug2("read - no more data, io:%p", pio);
				errno = 0;
				return 0;
			}
			return -1;
		}

		/* pick up APC if IO has completed */
		if (-1 == wait_for_any_event(NULL, 0, 0))
			return -1;

		if (w32_io_is_blocking(pio)) {
			while (fileio_is_io_available(pio, TRUE) == FALSE) {
				if (-1 == wait_for_any_event(NULL, 0, INFINITE))
					return -1;
			}
		}
		else if (pio->read_details.pending) {
			errno = EAGAIN;
			debug2("read - IO is pending, io:%p", pio);
			return -1;
		}
	}

	if (pio->read_details.error) {
		errno = errno_from_Win32Error(pio->read_details.error);
		/*write end of the pipe is closed or pipe broken or eof reached*/
		if ((pio->read_details.error == ERROR_BROKEN_PIPE) || 
			(pio->read_details.error == ERROR_HANDLE_EOF)) {
			debug2("read - (2) no more data, io:%p", pio);
			errno = 0;
			pio->read_details.error = 0;
			return 0;
		}
		debug("read - ERROR from cb :%d, io:%p", errno, pio);
		pio->read_details.error = 0;
		return -1;
	}

	bytes_copied = min(max, pio->read_details.remaining);
	memcpy(dst, pio->read_details.buf + pio->read_details.completed, bytes_copied);
	pio->read_details.remaining -= bytes_copied;
	pio->read_details.completed += bytes_copied;
	debug2("read - io:%p read: %d remaining: %d", pio, bytes_copied, 
	    pio->read_details.remaining);
	return bytes_copied;
}

VOID CALLBACK WriteCompletionRoutine(
	_In_    DWORD        dwErrorCode,
	_In_    DWORD        dwNumberOfBytesTransfered,
	_Inout_ LPOVERLAPPED lpOverlapped
	) {
	struct w32_io* pio = 
	    (struct w32_io*)((char*)lpOverlapped - offsetof(struct w32_io, write_overlapped));
	debug2("WriteCB - pio:%p, pending_state:%d, error:%d, transferred:%d of remaining: %d", 
	    pio, pio->write_details.pending, dwErrorCode, dwNumberOfBytesTransfered, 
	    pio->write_details.remaining);
	pio->write_details.error = dwErrorCode;
	/* TODO - assert that remaining == dwNumberOfBytesTransfered */
	if ((dwErrorCode == 0) && (pio->write_details.remaining != dwNumberOfBytesTransfered)) {
		debug("WriteCB - ERROR: broken assumption, io:%p, wrote:%d, remaining:%d", pio,
			dwNumberOfBytesTransfered, pio->write_details.remaining);
		DebugBreak();
	}	
	pio->write_details.remaining -= dwNumberOfBytesTransfered;
	pio->write_details.pending = FALSE;
	*((__int64*)&lpOverlapped->Offset) += dwNumberOfBytesTransfered;
}

/* write() implementation */
int 
fileio_write(struct w32_io* pio, const void *buf, unsigned int max) {
	int bytes_copied;
	HANDLE h = pio->handle;

	debug2("write - io:%p", pio);
	if ((pio->type == PIPE_FD) && (pio->internal.state == PIPE_READ_END)) {
		debug("write - ERROR: write called on a read end of pipe, io:%p", pio);
		errno = EBADF;
		return -1;
	}

	if (pio->write_details.pending) {
		if (w32_io_is_blocking(pio))
		{
			debug2("write - io pending, blocking call made, io:%p", pio);
			while (pio->write_details.pending) {
				if (wait_for_any_event(NULL, 0, INFINITE) == -1)
					return -1;
			}
		}
		else {
			errno = EAGAIN;
			debug2("write - IO is already pending, io:%p", pio);
			return -1;
		}
	}

	if (pio->write_details.error) {
		errno = errno_from_Win32Error(pio->write_details.error);
		debug("write - ERROR:%d on prior unblocking write, io:%p", errno, pio);
		pio->write_details.error = 0;
		return -1;
	}

	if (pio->write_details.buf == NULL) {
		pio->write_details.buf = malloc(WRITE_BUFFER_SIZE);
		if (pio->write_details.buf == NULL) {
			errno = ENOMEM;
			debug("write - ERROR:%d, io:%p", errno, pio);
			return -1;
		}
		pio->write_details.buf_size = WRITE_BUFFER_SIZE;
	}

	bytes_copied = min(max, pio->write_details.buf_size);
	memcpy(pio->write_details.buf, buf, bytes_copied);

	/* get underlying handle for standard io */
	if (pio->type == STD_IO_FD)
		h = GetStdHandle(pio->std_handle);

	if (WriteFileEx(h, pio->write_details.buf, bytes_copied, 
	    &pio->write_overlapped, &WriteCompletionRoutine)) {
		pio->write_details.pending = TRUE;
		pio->write_details.remaining = bytes_copied;
		/* execute APC if write has completed */
		if (wait_for_any_event(NULL, 0, 0) == -1)
			return -1;

		if (w32_io_is_blocking(pio)) {
			while (pio->write_details.pending) {
				if (wait_for_any_event(NULL, 0, INFINITE) == -1)
					return -1;
			}
		}
		if (!pio->write_details.pending && pio->write_details.error) {
			errno = errno_from_Win32Error(pio->write_details.error);
			debug("write - ERROR from cb:%d, io:%p", pio->write_details.error, pio);
			pio->write_details.error = 0;
			return -1;
		}
		debug2("write - reporting %d bytes written, io:%p", bytes_copied, pio);
		return bytes_copied;
	}
	else {
		errno = errno_from_Win32LastError();
		/* read end of the pipe closed    */
		if ((pio->type == PIPE_FD) && (errno == ERROR_NEGATIVE_SEEK)) {
			debug("write - ERROR:read end of the pipe closed, io:%p", pio);
			errno = EPIPE;
		}
		debug("write ERROR from cb(2):%d, io:%p", errno, pio);
		return -1;
	}

}

/* fstat() implemetation */
int 
fileio_fstat(struct w32_io* pio, struct _stat64 *buf) {

	int fd = _open_osfhandle((intptr_t)pio->handle, 0);
	debug2("fstat - pio:%p", pio);
	if (fd == -1) {
		errno = EOTHER;
		return -1;
	}

	return _fstat64(fd, buf);
}

int 
fileio_stat(const char *path, struct _stat64 *buf) {
	return _stat64(path, buf);
}

long 
fileio_lseek(struct w32_io* pio, long offset, int origin) {
	debug2("lseek - pio:%p", pio);
	if (origin != SEEK_SET) {
		debug("lseek - ERROR, origin is not supported %d", origin);
		errno = ENOTSUP;
		return -1;
	}

	//NO-OP as we automatically move file pointer in async io callbacks for files
	//assert current postion in overlapped struct
	return 0;
}

/* isatty() implementation */
int 
fileio_isatty(struct w32_io* pio) {
	if (GetFileType(pio->handle) == FILE_TYPE_CHAR)
		return 1; 
	else {
		errno = EINVAL;
		return 0;
	}
}

/* fdopen implementation */
FILE* 
fileio_fdopen(struct w32_io* pio, const char *mode) {

	int fd_flags = 0;
	debug2("fdopen - io:%p", pio);

	if (mode[1] == '\0') {
		switch (*mode) {
		case 'r':
			fd_flags = _O_RDONLY;
			break;
		case 'w':
			break;
		case 'a':
			fd_flags = _O_APPEND;
			break;
		default:
			errno = ENOTSUP;
			debug("fdopen - ERROR unsupported mode %s", mode);
			return NULL;
		}
	}
	else {
		errno = ENOTSUP;
		debug("fdopen - ERROR unsupported mode %s", mode);
		return NULL;
	}

	int fd = _open_osfhandle((intptr_t)pio->handle, fd_flags);

	if (fd == -1) {
		errno = EOTHER;
		debug("fdopen - ERROR:%d _open_osfhandle()", errno);
		return NULL;
	}

	return _fdopen(fd, mode);
}

int 
fileio_on_select(struct w32_io* pio, BOOL rd) {

	if (!rd)
		return 0;

	if (!pio->read_details.pending && !fileio_is_io_available(pio, rd))
		return fileio_ReadFileEx(pio);

	return 0;
}


int 
fileio_close(struct w32_io* pio) {
	debug2("fileclose - pio:%p", pio);
	CancelIo(pio->handle);
	//let queued APCs (if any) drain
	SleepEx(0, TRUE);
	if (pio->type != STD_IO_FD) {//STD handles are never explicitly closed
		CloseHandle(pio->handle);

		if (pio->read_details.buf)
			free(pio->read_details.buf);

		if (pio->write_details.buf)
			free(pio->write_details.buf);

		free(pio);
	}
	return 0;
}

BOOL 
fileio_is_io_available(struct w32_io* pio, BOOL rd) {
	if (rd) {
		if (pio->read_details.remaining || pio->read_details.error)
			return TRUE;
		else
			return FALSE;
	}
	else { //write
		return (pio->write_details.pending == FALSE) ? TRUE : FALSE;
	}
}