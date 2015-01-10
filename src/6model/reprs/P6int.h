/* Numeric codes used used at compose-time to signal that bitsize should be
 * computed from C types. */
#define MVM_P6INT_CTYPE_INT  1
#define MVM_P6INT_CTYPE_LONG 2

/* Representation used by P6 native ints. */
struct MVMP6intBody {
    /* Integer storage slot. */
    union {
        MVMint64 i64;
        MVMint32 i32;
        MVMint16 i16;
        MVMint8  i8;
    } value;
};
struct MVMP6int {
    MVMObject common;
    MVMP6intBody body;
};

/* The bit width requirement is shared for all instances of the same type. */
struct MVMP6intREPRData {
    MVMint16       bits;
    MVMint16       is_unsigned;
    MVMStorageSpec storage_spec;
};

/* Function for REPR setup. */
const MVMREPROps * MVMP6int_initialize(MVMThreadContext *tc);
