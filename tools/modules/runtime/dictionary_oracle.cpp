#include "ac1/core/file.h"
#include "ac1/dict/dict.h"
#include <stdio.h>

// The audit reads the production dictionary module's actual lookup results.
// Python supplies canonical query keys, but cannot decide the resulting values,
// fallback expansion, pool limits or repeated-translation stability for this export.
int main(int argc, char** argv)
{
    if (argc != 4) return 2;
    unsigned char* bytes=NULL;
    int count=0;
    if (!ac1::core::file_read_all(argv[1],&bytes,&count,ac1::core::FILE_READ_MAX_DEFAULT)) {
        fprintf(stderr,"dictionary read failed: input unreadable, empty or exceeds %d bytes\n",
            ac1::core::FILE_READ_MAX_DEFAULT);
        return 3;
    }
    // Use package startup's acceptance rules, including rejection of partial capacity loads.
    const bool accepted=ac1::dict::init_bytes(bytes,count);
    ac1::core::file_free(bytes);
    printf("%s\n",ac1::dict::status());
    if (!accepted) return 3;
    const ac1::dict::DictStats& stats=ac1::dict::stats();
    FILE* queries=NULL;
    FILE* output=NULL;
    if (fopen_s(&queries,argv[2],"rb") || !queries) return 2;
    if (fopen_s(&output,argv[3],"wb") || !output) { fclose(queries); return 2; }
    const unsigned header[]={
        0x4F444341,2,ac1::dict::RULES_VERSION,(unsigned)stats.keys,(unsigned)stats.keypool_used,(unsigned)stats.valpool_used,
        (unsigned)stats.sup_lines,(unsigned)stats.sup_new,(unsigned)stats.sup_duplicate,
        (unsigned)stats.sup_bad,(unsigned)stats.sup_keybad,(unsigned)stats.sup_valbad,
        (unsigned)stats.sup_poolfull,(unsigned)stats.sup_keyfull,(unsigned)stats.sup_keylong,
        (unsigned)stats.sup_vallong,(unsigned)stats.sup_fallback_full
    };
    bool ok=fwrite(header,sizeof(header),1,output) == 1;
    unsigned exported=0, missing=0, size=0;
    wchar_t key[DICT_KEY_MAX+1]={};
    while (ok && fread(&size,sizeof(size),1,queries) == 1) {
        if (!size || size > DICT_KEY_MAX || fread(key,2,size,queries) != size) { ok=false; break; }
        const ac1::dict::DictVal value=ac1::dict::lookup(key,(int)size);
        const unsigned lengths[]={size,(unsigned)value.len};
        ok=fwrite(lengths,sizeof(lengths),1,output) == 1 && fwrite(key,2,size,output) == size;
        if (value.p && value.len > 0) {
            ok=ok && fwrite(value.p,2,(unsigned)value.len,output) == (unsigned)value.len;
            ++exported;
        } else ++missing;
    }
    ok=ok && !ferror(queries) && !ferror(output);
    fclose(queries);
    ok=fclose(output) == 0 && ok;
    printf("dictionary keys=%d exported=%u missing=%u key_pool=%d value_pool=%d\n",
        stats.keys,exported,missing,stats.keypool_used,stats.valpool_used);
    return ok && !missing && exported == (unsigned)stats.keys ? 0 : 1;
}
