use std::fs::File;
use std::io::Write;
use flate2::write::GzEncoder;
use flate2::Compression;
use std::convert::TryInto;
use std::io::Error;
use std::iter::Iterator;
use std::path::Path;
use std::fs::Metadata;
use std::os::fd::AsRawFd;
use std::sync::atomic::{AtomicUsize, Ordering};

// The below is copied from:
// https://github.com/RazrFalcon/memmap2-rs/blob/master/src/unix.rs
// Apache2 or MIT licensed
//
// Our primary goal is to avoid an extra `statx` syscall on each mmapped file.

fn page_size() -> usize {
    static PAGE_SIZE: AtomicUsize = AtomicUsize::new(0);

    match PAGE_SIZE.load(Ordering::Relaxed) {
        0 => {
            let page_size = unsafe { libc::sysconf(libc::_SC_PAGESIZE) as usize };

            PAGE_SIZE.store(page_size, Ordering::Relaxed);

            page_size
        }
        page_size => page_size,
    }
}

pub struct Mmap {
    ptr: *mut libc::c_void,
    len: usize,
}

impl Drop for Mmap {
    fn drop(&mut self) {
        // Any errors during unmapping/closing are ignored as the only way
        // to report them would be through panicking which is highly discouraged
        // in Drop impls, c.f. https://github.com/rust-lang/lang-team/issues/97
        unsafe { libc::munmap(self.ptr, self.len as libc::size_t) };
    }
}

// End copy

fn mmap(file: &File, length: usize) -> Result<Mmap, Error> {
    let fd = file.as_raw_fd();
    let ptr = unsafe { libc::mmap(
        std::ptr::null_mut(),
        length,
        0,
        libc::MAP_PRIVATE,
        fd,
        0,
    ) };

    if ptr == libc::MAP_FAILED {
        Err(Error::last_os_error())
    } else {
        Ok(Mmap { ptr: ptr, len: length})
    }
}


fn mincore(slice: &Mmap) -> Result<Vec<u8>, Error> {
    let page_size = page_size();
    let pages = (slice.len + page_size - 1) / page_size;
    let mut buffer = vec![0_u8; pages];

    let ret = unsafe { libc::mincore(slice.ptr, slice.len, buffer.as_mut_ptr()) };
    if ret == 0 {
        return Ok(buffer)
    }

    Err(Error::last_os_error())
}

fn dump_file(encoder: &mut impl Write, path: &Path, metadata: &Metadata) -> Result<(), Error> {
    let page_size = page_size();
    let input_file = File::open(path)?;

    let mmap = mmap(&input_file, metadata.len().try_into().unwrap())?;

    let mut wrote_header = false;
    let mut last = 0;
    let mut offset = 0;
    let pages = 1<<20;
    loop {
        let start = offset * page_size;
        if start >= mmap.len {
            break;
        }

        let length = std::cmp::min(pages * page_size, mmap.len - start);
        let istart = start.try_into().unwrap();
        let slice = Mmap { 
            ptr: unsafe { mmap.ptr.offset(istart) },
            len: length
        };

        for (chunk_pos, e) in mincore(&slice)?.into_iter().enumerate() {
            let pos = chunk_pos + offset;
            if e == 0 {
                continue;
            }

            // Write the difference to the file, rather than the whole number
            // This improves gzip's compression ratio
            let diff = (pos - last).to_string();
            last = pos;

            if !wrote_header {
                encoder.write_all(path.to_string_lossy().as_bytes())?;
                encoder.write_all(b"\n")?;
                wrote_header = true;
            }

            encoder.write_all(diff.as_bytes())?;
            encoder.write_all(b"\n")?;
        }
        offset += pages;
    }

    Ok(())
}

fn spider(output: &mut impl Write, directory: &Path) -> std::io::Result<()> {
    for entry in directory.read_dir()? {
        let path = entry?.path();
        let metadata = path.metadata()?;

        if metadata.is_dir() {
            spider(output, &path)?;
        } else if metadata.is_file() && metadata.len() > 0 {
            dump_file(output, &path, &metadata)?;
        }
    }

    Ok(())
}

fn main() -> std::io::Result<()> {
    let mut encoder = {
        let output_file = File::create(".happycache.gz")?;
        GzEncoder::new(output_file, Compression::default())
    };

    spider(&mut encoder, Path::new("."))?;
    encoder.finish()?;

    Ok(())
}
