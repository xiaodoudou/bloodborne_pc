// Texture cache subresource extents: containment against the defaulted ordering, which
// compares levels before layers (shadps4-emu/shadPS4#5457).
#include <algorithm>
#include <cassert>
#include <cstdio>

#include "video_core/texture_cache/types.h"

using VideoCore::SubresourceExtent;

int main() {
    // The ordering is lexicographic, not containment: these are what the cache got wrong.
    const SubresourceExtent mips4_layers8{4, 8};
    const SubresourceExtent mips1_layers256{1, 256};
    const SubresourceExtent mips1_layers9{1, 9};
    assert(!(mips1_layers256 > mips4_layers8));
    assert(!(mips4_layers8 < mips1_layers9));

    // Neither covers the other when one has more mips and the other more layers.
    assert(!mips4_layers8.Covers(mips1_layers256));
    assert(!mips1_layers256.Covers(mips4_layers8));
    assert(!mips4_layers8.Covers(mips1_layers9));
    assert(!mips1_layers9.Covers(mips4_layers8));

    // Supersets in both counts, and equality.
    assert(mips4_layers8.Covers(mips4_layers8));
    assert(mips4_layers8.Covers(SubresourceExtent{1, 8}));
    assert(mips4_layers8.Covers(SubresourceExtent{4, 1}));
    assert(mips4_layers8.Covers(SubresourceExtent{}));
    assert(!SubresourceExtent{}.Covers(SubresourceExtent{1, 6}));
    assert((SubresourceExtent{1, 6}.Covers(SubresourceExtent{})));

    // Max takes each count apart; std::max keeps one extent whole and drops the other's layers.
    const SubresourceExtent cube{1, 6};
    const SubresourceExtent mips4{4, 1};
    assert((std::max(cube, mips4) == SubresourceExtent{4, 1}));
    assert((cube.Max(mips4) == SubresourceExtent{4, 6}));
    assert((mips4.Max(cube) == SubresourceExtent{4, 6}));
    assert((cube.Max(cube) == cube));
    assert(cube.Max(mips4).Covers(cube) && cube.Max(mips4).Covers(mips4));

    std::puts("texture cache types: ok");
    return 0;
}
