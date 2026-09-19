# Every producer reports one allocation size, never below the file size

Every producer of file info reports an allocation size, and a filesystem host sizes a mapped section from that number. One helper returns the larger of the real allocation, when a handle can query it, and the file size rounded up to 4 KiB. We rejected the real allocation alone because a metacopy shell, a sparse file, and a compressed file allocate less than their size, so a section sized from it is too short.
