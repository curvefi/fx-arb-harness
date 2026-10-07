# Test fixtures

`block-tape-v1.npz`: four blocks in the format `data/market/build_block_tape.py` writes
(uncompressed NPZ, little-endian). `price[k][o] = 100 + k + 0.1 o`, all volumes 1.

```python
import numpy as np
k = np.arange(4)
np.savez('block-tape-v1.npz',
         format_version=np.array(1, '<u4'), t0=np.array(1_700_000_003, '<i8'), block_s=np.array(12, '<i8'),
         price_offsets_s=np.arange(5, dtype='<i8'), volume_offsets_s=np.arange(3, dtype='<i8'),
         price=(100.0 + k[:, None] + 0.1 * np.arange(5)[None, :]).astype('<f8'), volume=np.ones((4, 3), '<f8'))
```
