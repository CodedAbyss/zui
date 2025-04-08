#ifdef TYPES
#define FST(N,...) N
#define CDR(N,...) __VA_ARGS__
#define EXTRACT(N) FST N
#define EXTRACT_CDR(N) CDR N
#define CONCAT(name,ext) name##_##ext
#define CREATE(name,ext) CONCAT(name,ext)
#define TYPE EXTRACT((TYPES))
#define REST EXTRACT_CDR((TYPES))
int CREATE(func,EXTRACT((TYPES)))();

#endif
