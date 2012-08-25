/* Fixed size rational numbers exposed to Python */


#include <stdint.h>
#include <math.h>
//#include <Python/Python.h>
//#include <Python/structmember.h>
#include <Python.h>
#include <structmember.h>
#include <numpy/arrayobject.h>
#include <numpy/ufuncobject.h>

/* Relevant arithmetic exceptions */

/* Uncomment the following line to work around a bug in numpy */
/* #define ACQUIRE_GIL */

static void
set_overflow(void) {
#ifdef ACQUIRE_GIL
    /* Need to grab the GIL to dodge a bug in numpy */
    PyGILState_STATE state = PyGILState_Ensure();
#endif
    if (!PyErr_Occurred()) {
        PyErr_SetString(PyExc_OverflowError,
                "overflow in rational arithmetic");
    }
#ifdef ACQUIRE_GIL
    PyGILState_Release(state);
#endif
}

static void
set_zero_divide(void) {
#ifdef ACQUIRE_GIL
    /* Need to grab the GIL to dodge a bug in numpy */
    PyGILState_STATE state = PyGILState_Ensure();
#endif
    if (!PyErr_Occurred()) {
        PyErr_SetString(PyExc_ZeroDivisionError,
                "zero divide in rational arithmetic");
    }
#ifdef ACQUIRE_GIL
    PyGILState_Release(state);
#endif
}

/* Integer arithmetic utilities */

static NPY_INLINE int32_t
safe_neg(int32_t x) {
    if (x==(int32_t)1<<31) {
        set_overflow();
    }
    return -x;
}

static NPY_INLINE int32_t
safe_abs32(int32_t x) {
    if (x>=0) {
        return x;
    }
    int32_t nx = -x;
    if (nx<0) {
        set_overflow();
    }
    return nx;
}

static NPY_INLINE int64_t
safe_abs64(int64_t x) {
    if (x>=0) {
        return x;
    }
    int64_t nx = -x;
    if (nx<0) {
        set_overflow();
    }
    return nx;
}

static NPY_INLINE int64_t
gcd(int64_t x, int64_t y) {
    x = safe_abs64(x);
    y = safe_abs64(y);
    if (x < y) {
        int64_t t = x;
        x = y;
        y = t;
    }
    while (y) {
        x = x%y;
        int64_t t = x;
        x = y;
        y = t;
    }
    return x;
}

static NPY_INLINE int64_t
lcm(int64_t x, int64_t y) {
    if (!x || !y) {
        return 0;
    }
    x /= gcd(x,y);
    int64_t lcm = x*y;
    if (lcm/y!=x) {
        set_overflow();
    }
    return safe_abs64(lcm);
}

/* Fixed precision rational numbers */

typedef struct {
    /* numerator */
    int32_t n;
    /*
     * denominator minus one: numpy.zeros() uses memset(0) for non-object
     * types, so need to ensure that rational(0) has all zero bytes
     */
    int32_t dmm;
} rational;

static NPY_INLINE rational
make_rational_int(int64_t n) {
    rational r = {n,0};
    if (r.n != n) {
        set_overflow();
    }
    return r;
}

static rational
make_rational_slow(int64_t n_, int64_t d_) {
    rational r = {0};
    if (!d_) {
        set_zero_divide();
    }
    else {
        int64_t g = gcd(n_,d_);
        n_ /= g;
        d_ /= g;
        r.n = n_;
        int32_t d = d_;
        if (r.n!=n_ || d!=d_) {
            set_overflow();
        }
        else {
            if (d <= 0) {
                d = -d;
                r.n = safe_neg(r.n);
            }
            r.dmm = d-1;
        }
    }
    return r;
}

static NPY_INLINE int32_t
d(rational r) {
    return r.dmm+1;
}

/* Assumes d_ > 0 */
static rational
make_rational_fast(int64_t n_, int64_t d_) {
    int64_t g = gcd(n_,d_);
    n_ /= g;
    d_ /= g;
    rational r;
    r.n = n_;
    r.dmm = d_-1;
    if (r.n!=n_ || r.dmm+1!=d_) {
        set_overflow();
    }
    return r;
}

static NPY_INLINE rational
rational_negative(rational r) {
    rational x;
    x.n = safe_neg(r.n);
    x.dmm = r.dmm;
    return x;
}

static NPY_INLINE rational
rational_add(rational x, rational y) {
    /*
     * Note that the numerator computation can never overflow int128_t,
     * since each term is strictly under 2**128/4 (since d > 0).
     */
    return make_rational_fast((int64_t)x.n*d(y)+(int64_t)d(x)*y.n,(int64_t)d(x)*d(y));
}

static NPY_INLINE rational
rational_subtract(rational x, rational y) {
    /* We're safe from overflow as with + */
    return make_rational_fast((int64_t)x.n*d(y)-(int64_t)d(x)*y.n,(int64_t)d(x)*d(y));
}

static NPY_INLINE rational
rational_multiply(rational x, rational y) {
    /* We're safe from overflow as with + */
    return make_rational_fast((int64_t)x.n*y.n,(int64_t)d(x)*d(y));
}

static NPY_INLINE rational
rational_divide(rational x, rational y) {
    return make_rational_slow((int64_t)x.n*d(y),(int64_t)d(x)*y.n);
}

static NPY_INLINE int64_t
rational_floor(rational x) {
    /* Always round down */
    if (x.n>=0) {
        return x.n/d(x);
    }
    /*
     * This can be done without casting up to 64 bits, but it requires
     * working out all the sign cases
     */
    return -((-(int64_t)x.n+d(x)-1)/d(x));
}

static NPY_INLINE int64_t
rational_ceil(rational x) {
    return -rational_floor(rational_negative(x));
}

static NPY_INLINE rational
rational_remainder(rational x, rational y) {
    return rational_subtract(x, rational_multiply(y,make_rational_int(
                    rational_floor(rational_divide(x,y)))));
}

static NPY_INLINE rational
rational_abs(rational x) {
    rational y;
    y.n = safe_abs32(x.n);
    y.dmm = x.dmm;
    return y;
}

static NPY_INLINE int64_t
rational_rint(rational x) {
    /*
     * Round towards nearest integer, moving exact half integers towards
     * zero
     */
    int32_t d_ = d(x);
    return (2*(int64_t)x.n+(x.n<0?-d_:d_))/(2*(int64_t)d_);
}

static NPY_INLINE int
rational_sign(rational x) {
    return x.n<0?-1:x.n==0?0:1;
}

static NPY_INLINE rational
rational_inverse(rational x) {
    rational y = {0};
    if (!x.n) {
        set_zero_divide();
    }
    else {
        y.n = d(x);
        int32_t d = x.n;
        if (d <= 0) {
            d = safe_neg(d);
            y.n = -y.n;
        }
        y.dmm = d-1;
    }
    return y;
}

static NPY_INLINE int
rational_eq(rational x, rational y) {
    /*
     * Since we enforce d > 0, and store fractions in reduced form,
     * equality is easy.
     */
    return x.n==y.n && x.dmm==y.dmm;
}

static NPY_INLINE int
rational_ne(rational x, rational y) {
    return !rational_eq(x,y);
}

static NPY_INLINE int
rational_lt(rational x, rational y) {
    return (int64_t)x.n*d(y) < (int64_t)y.n*d(x);
}

static NPY_INLINE int
rational_gt(rational x, rational y) {
    return rational_lt(y,x);
}

static NPY_INLINE int
rational_le(rational x, rational y) {
    return !rational_lt(y,x);
}

static NPY_INLINE int
rational_ge(rational x, rational y) {
    return !rational_lt(x,y);
}

static NPY_INLINE int32_t
rational_int(rational x) {
    return x.n/d(x);
}

static NPY_INLINE double
rational_double(rational x) {
    return (double)x.n/d(x);
}

static NPY_INLINE int
rational_nonzero(rational x) {
    return x.n!=0;
}

static int
scan_rational(const char** s, rational* x) {
    long n,d;
    int offset;
    if (sscanf(*s,"%ld%n",&n,&offset)<=0) {
        return 0;
    }
    const char* ss = *s+offset;
    if (*ss!='/') {
        *s = ss;
        *x = make_rational_int(n);
        return 1;
    }
    ss++;
    if (sscanf(ss,"%ld%n",&d,&offset)<=0 || d<=0) {
        return 0;
    }
    *s = ss+offset;
    *x = make_rational_slow(n,d);
    return 1;
}

/* Expose rational to Python as a numpy scalar */

typedef struct {
    PyObject_HEAD;
    rational r;
} PyRational;

static PyTypeObject PyRational_Type;

static NPY_INLINE int
PyRational_Check(PyObject* object) {
    return PyObject_IsInstance(object,(PyObject*)&PyRational_Type);
}

static PyObject*
PyRational_FromRational(rational x) {
    PyRational* p = (PyRational*)PyRational_Type.tp_alloc(&PyRational_Type,0);
    if (p) {
        p->r = x;
    }
    return (PyObject*)p;
}

static PyObject*
pyrational_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
    if (kwds && PyDict_Size(kwds)) {
        PyErr_SetString(PyExc_TypeError,
                "constructor takes no keyword arguments");
        return 0;
    }
    Py_ssize_t size = PyTuple_GET_SIZE(args);
    if (size>2) {
        PyErr_SetString(PyExc_TypeError,
                "expected rational or numerator and optional denominator");
        return 0;
    }
    PyObject* x[2] = {PyTuple_GET_ITEM(args,0),PyTuple_GET_ITEM(args,1)};
    if (size==1) {
        if (PyRational_Check(x[0])) {
            Py_INCREF(x[0]);
            return x[0];
        }
        else if (PyString_Check(x[0])) {
            const char* s = PyString_AS_STRING(x[0]);
            rational x;
            if (scan_rational(&s,&x)) {
                const char* p;
                for (p = s; *p; p++) {
                    if (!isspace(*p)) {
                        goto bad;
                    }
                }
                return PyRational_FromRational(x);
            }
            bad:
            PyErr_Format(PyExc_ValueError,
                    "invalid rational literal '%s'",s);
            return 0;
        }
    }
    long n[2]={0,1};
    int i;
    for (i=0;i<size;i++) {
        n[i] = PyInt_AsLong(x[i]);
        if (n[i]==-1 && PyErr_Occurred()) {
            if (PyErr_ExceptionMatches(PyExc_TypeError)) {
                PyErr_Format(PyExc_TypeError,
                        "expected integer %s, got %s",
                        (i ? "denominator" : "numerator"),
                        x[i]->ob_type->tp_name);
            }
            return 0;
        }
        /* Check that we had an exact integer */
        PyObject* y = PyInt_FromLong(n[i]);
        if (!y) {
            return 0;
        }
        int eq = PyObject_RichCompareBool(x[i],y,Py_EQ);
        Py_DECREF(y);
        if (eq<0) {
            return 0;
        }
        if (!eq) {
            PyErr_Format(PyExc_TypeError,
                    "expected integer %s, got %s",
                    (i ? "denominator" : "numerator"),
                    x[i]->ob_type->tp_name);
            return 0;
        }
    }
    rational r = make_rational_slow(n[0],n[1]);
    if (PyErr_Occurred()) {
        return 0;
    }
    return PyRational_FromRational(r);
}

/*
 * Returns Py_NotImplemented on most conversion failures, or raises an
 * overflow error for too long ints
 */
#define AS_RATIONAL(dst,object) \
    rational dst = {0}; \
    if (PyRational_Check(object)) { \
        dst = ((PyRational*)object)->r; \
    } \
    else { \
        long n_ = PyInt_AsLong(object); \
        if (n_==-1 && PyErr_Occurred()) { \
            if (PyErr_ExceptionMatches(PyExc_TypeError)) { \
                PyErr_Clear(); \
                Py_INCREF(Py_NotImplemented); \
                return Py_NotImplemented; \
            } \
            return 0; \
        } \
        PyObject* y_ = PyInt_FromLong(n_); \
        if (!y_) { \
            return 0; \
        } \
        int eq_ = PyObject_RichCompareBool(object,y_,Py_EQ); \
        Py_DECREF(y_); \
        if (eq_<0) { \
            return 0; \
        } \
        if (!eq_) { \
            Py_INCREF(Py_NotImplemented); \
            return Py_NotImplemented; \
        } \
        dst = make_rational_int(n_); \
    }

static PyObject*
pyrational_richcompare(PyObject* a, PyObject* b, int op) {
    AS_RATIONAL(x,a);
    AS_RATIONAL(y,b);
    int result = 0;
    #define OP(py,op) case py: result = rational_##op(x,y); break;
    switch (op) {
        OP(Py_LT,lt)
        OP(Py_LE,le)
        OP(Py_EQ,eq)
        OP(Py_NE,ne)
        OP(Py_GT,gt)
        OP(Py_GE,ge)
    };
    #undef OP
    return PyBool_FromLong(result);
}

static PyObject*
pyrational_repr(PyObject* self) {
    rational x = ((PyRational*)self)->r;
    if (d(x)!=1) {
        return PyString_FromFormat(
                "rational(%ld,%ld)",(long)x.n,(long)d(x));
    }
    else {
        return PyString_FromFormat(
                "rational(%ld)",(long)x.n);
    }
}

static PyObject*
pyrational_str(PyObject* self) {
    rational x = ((PyRational*)self)->r;
    if (d(x)!=1) {
        return PyString_FromFormat(
                "%ld/%ld",(long)x.n,(long)d(x));
    }
    else {
        return PyString_FromFormat(
                "%ld",(long)x.n);
    }
}

static long
pyrational_hash(PyObject* self) {
    rational x = ((PyRational*)self)->r;
    /* Use a fairly weak hash as Python expects */
    long h = 131071*x.n+524287*x.dmm;
    /* Never return the special error value -1 */
    return h==-1?2:h;
}

#define RATIONAL_BINOP_2(name,exp) \
    static PyObject* \
    pyrational_##name(PyObject* a, PyObject* b) { \
        AS_RATIONAL(x,a); \
        AS_RATIONAL(y,b); \
        rational z = exp; \
        if (PyErr_Occurred()) { \
            return 0; \
        } \
        return PyRational_FromRational(z); \
    }
#define RATIONAL_BINOP(name) RATIONAL_BINOP_2(name,rational_##name(x,y))
RATIONAL_BINOP(add)
RATIONAL_BINOP(subtract)
RATIONAL_BINOP(multiply)
RATIONAL_BINOP(divide)
RATIONAL_BINOP(remainder)
RATIONAL_BINOP_2(floor_divide,make_rational_int(rational_floor(rational_divide(x,y))))

#define RATIONAL_UNOP(name,type,exp,convert) \
    static PyObject* \
    pyrational_##name(PyObject* self) { \
        rational x = ((PyRational*)self)->r; \
        type y = exp; \
        if (PyErr_Occurred()) { \
            return 0; \
        } \
        return convert(y); \
    }
RATIONAL_UNOP(negative,rational,rational_negative(x),PyRational_FromRational)
RATIONAL_UNOP(absolute,rational,rational_abs(x),PyRational_FromRational)
RATIONAL_UNOP(int,long,rational_int(x),PyInt_FromLong)
RATIONAL_UNOP(float,double,rational_double(x),PyFloat_FromDouble)

static PyObject*
pyrational_positive(PyObject* self) {
    Py_INCREF(self);
    return self;
}

static int
pyrational_nonzero(PyObject* self) {
    rational x = ((PyRational*)self)->r;
    return rational_nonzero(x);
}

static PyNumberMethods pyrational_as_number = {
    pyrational_add,          /* nb_add */
    pyrational_subtract,     /* nb_subtract */
    pyrational_multiply,     /* nb_multiply */
    pyrational_divide,       /* nb_divide */
    pyrational_remainder,    /* nb_remainder */
    0,                       /* nb_divmod */
    0,                       /* nb_power */
    pyrational_negative,     /* nb_negative */
    pyrational_positive,     /* nb_positive */
    pyrational_absolute,     /* nb_absolute */
    pyrational_nonzero,      /* nb_nonzero */
    0,                       /* nb_invert */
    0,                       /* nb_lshift */
    0,                       /* nb_rshift */
    0,                       /* nb_and */
    0,                       /* nb_xor */
    0,                       /* nb_or */
    0,                       /* nb_coerce */
    pyrational_int,          /* nb_int */
    pyrational_int,          /* nb_long */
    pyrational_float,        /* nb_float */
    0,                       /* nb_oct */
    0,                       /* nb_hex */

    0,                       /* nb_inplace_add */
    0,                       /* nb_inplace_subtract */
    0,                       /* nb_inplace_multiply */
    0,                       /* nb_inplace_divide */
    0,                       /* nb_inplace_remainder */
    0,                       /* nb_inplace_power */
    0,                       /* nb_inplace_lshift */
    0,                       /* nb_inplace_rshift */
    0,                       /* nb_inplace_and */
    0,                       /* nb_inplace_xor */
    0,                       /* nb_inplace_or */

    pyrational_floor_divide, /* nb_floor_divide */
    pyrational_divide,       /* nb_true_divide */
    0,                       /* nb_inplace_floor_divide */
    0,                       /* nb_inplace_true_divide */
    0,                       /* nb_index */
};

static PyObject*
pyrational_n(PyObject* self, void* closure) {
    return PyInt_FromLong(((PyRational*)self)->r.n);
}

static PyObject*
pyrational_d(PyObject* self, void* closure) {
    return PyInt_FromLong(d(((PyRational*)self)->r));
}

static PyGetSetDef pyrational_getset[] = {
    {(char*)"n",pyrational_n,0,(char*)"numerator",0},
    {(char*)"d",pyrational_d,0,(char*)"denominator",0},
    {0} /* sentinel */
};

static PyTypeObject PyRational_Type = {
    PyObject_HEAD_INIT(&PyType_Type)
    0,                                        /* ob_size */
    "rational",                               /* tp_name */
    sizeof(PyRational),                       /* tp_basicsize */
    0,                                        /* tp_itemsize */
    0,                                        /* tp_dealloc */
    0,                                        /* tp_print */
    0,                                        /* tp_getattr */
    0,                                        /* tp_setattr */
    0,                                        /* tp_compare */
    pyrational_repr,                          /* tp_repr */
    &pyrational_as_number,                    /* tp_as_number */
    0,                                        /* tp_as_sequence */
    0,                                        /* tp_as_mapping */
    pyrational_hash,                          /* tp_hash */
    0,                                        /* tp_call */
    pyrational_str,                           /* tp_str */
    0,                                        /* tp_getattro */
    0,                                        /* tp_setattro */
    0,                                        /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_CHECKTYPES, /* tp_flags */
    "Fixed precision rational numbers",       /* tp_doc */
    0,                                        /* tp_traverse */
    0,                                        /* tp_clear */
    pyrational_richcompare,                   /* tp_richcompare */
    0,                                        /* tp_weaklistoffset */
    0,                                        /* tp_iter */
    0,                                        /* tp_iternext */
    0,                                        /* tp_methods */
    0,                                        /* tp_members */
    pyrational_getset,                        /* tp_getset */
    0,                                        /* tp_base */
    0,                                        /* tp_dict */
    0,                                        /* tp_descr_get */
    0,                                        /* tp_descr_set */
    0,                                        /* tp_dictoffset */
    0,                                        /* tp_init */
    0,                                        /* tp_alloc */
    pyrational_new,                           /* tp_new */
    0,                                        /* tp_free */
};

/* Numpy support */

static PyObject*
npyrational_getitem(void* data, void* arr) {
    rational r;
    memcpy(&r,data,sizeof(rational));
    return PyRational_FromRational(r);
}

static int
npyrational_setitem(PyObject* item, void* data, void* arr) {
    rational r;
    if (PyRational_Check(item)) {
        r = ((PyRational*)item)->r;
    }
    else {
        long n = PyInt_AsLong(item);
        if (n==-1 && PyErr_Occurred()) {
            return -1;
        }
        PyObject* y = PyInt_FromLong(n);
        if (!y) {
            return -1;
        }
        int eq = PyObject_RichCompareBool(item,y,Py_EQ);
        Py_DECREF(y);
        if (eq<0) {
            return -1;
        }
        if (!eq) {
            PyErr_Format(PyExc_TypeError,
                    "expected rational, got %s", item->ob_type->tp_name);
            return -1;
        }
        r = make_rational_int(n);
    }
    memcpy(data,&r,sizeof(rational));
    return 0;
}

static NPY_INLINE void
byteswap(int32_t* x) {
    char* p = (char*)x;
    size_t i;
    for (i = 0; i < sizeof(*x)/2; i++) {
        int j = sizeof(*x)-1-i;
        char t = p[i];
        p[i] = p[j];
        p[j] = t;
    }
}

static void
npyrational_copyswapn(void* dst_, npy_intp dstride, void* src_,
        npy_intp sstride, npy_intp n, int swap, void* arr) {
    char *dst = (char*)dst_, *src = (char*)src_;
    if (!src) {
        return;
    }
    npy_intp i;
    if (swap) {
        for (i = 0; i < n; i++) {
            rational* r = (rational*)(dst+dstride*i);
            memcpy(r,src+sstride*i,sizeof(rational));
            byteswap(&r->n);
            byteswap(&r->dmm);
        }
    }
    else if (dstride == sizeof(rational) && sstride == sizeof(rational)) {
        memcpy(dst, src, n*sizeof(rational));
    }
    else {
        for (i = 0; i < n; i++) {
            memcpy(dst + dstride*i, src + sstride*i, sizeof(rational));
        }
    }
}

static void
npyrational_copyswap(void* dst, void* src, int swap, void* arr) {
    if (!src) {
        return;
    }
    rational* r = (rational*)dst;
    memcpy(r,src,sizeof(rational));
    if (swap) {
        byteswap(&r->n);
        byteswap(&r->dmm);
    }
}

static int
npyrational_compare(const void* d0, const void* d1, void* arr) {
    rational x = *(rational*)d0,
             y = *(rational*)d1;
    return rational_lt(x,y)?-1:rational_eq(x,y)?0:1;
}

#define FIND_EXTREME(name,op) \
    static int \
    npyrational_##name(void* data_, npy_intp n, npy_intp* max_ind, void* arr) { \
        if (!n) { \
            return 0; \
        } \
        const rational* data = (rational*)data_; \
        npy_intp best_i = 0; \
        rational best_r = data[0]; \
        npy_intp i; \
        for (i = 1; i < n; i++) { \
            if (rational_##op(data[i],best_r)) { \
                best_i = i; \
                best_r = data[i]; \
            } \
        } \
        *max_ind = best_i; \
        return 0; \
    }
FIND_EXTREME(argmin,lt)
FIND_EXTREME(argmax,gt)

static void
npyrational_dot(void* ip0_, npy_intp is0, void* ip1_, npy_intp is1,
        void* op, npy_intp n, void* arr) {
    rational r = {0};
    const char *ip0 = (char*)ip0_, *ip1 = (char*)ip1_;
    npy_intp i;
    for (i = 0; i < n; i++) {
        r = rational_add(r,rational_multiply(*(rational*)ip0,*(rational*)ip1));
        ip0 += is0;
        ip1 += is1;
    }
    *(rational*)op = r;
}

static npy_bool
npyrational_nonzero(void* data, void* arr) {
    rational r;
    memcpy(&r,data,sizeof(r));
    return rational_nonzero(r)?NPY_TRUE:NPY_FALSE;
}

static int
npyrational_fill(void* data_, npy_intp length, void* arr) {
    rational* data = (rational*)data_;
    rational delta = rational_subtract(data[1],data[0]);
    rational r = data[1];
    npy_intp i;
    for (i = 2; i < length; i++) {
        r = rational_add(r,delta);
        data[i] = r;
    }
    return 0;
}

static int
npyrational_fillwithscalar(void* buffer_, npy_intp length,
        void* value, void* arr) {
    rational r = *(rational*)value;
    rational* buffer = (rational*)buffer_;
    npy_intp i;
    for (i = 0; i < length; i++) {
        buffer[i] = r;
    }
    return 0;
}

static PyArray_ArrFuncs npyrational_arrfuncs;

typedef struct { char c; rational r; } align_test;

PyArray_Descr npyrational_descr = {
    PyObject_HEAD_INIT(0)
    &PyRational_Type,       /* typeobj */
    'V',                    /* kind */
    'r',                    /* type */
    '=',                    /* byteorder */
    /*
     * For now, we need NPY_NEEDS_PYAPI in order to make numpy detect our
     * exceptions.  This isn't technically necessary,
     * since we're careful about thread safety, and hopefully future
     * versions of numpy will recognize that.
     */
    NPY_NEEDS_PYAPI | NPY_USE_GETITEM | NPY_USE_SETITEM, /* hasobject */
    0,                      /* type_num */
    sizeof(rational),       /* elsize */
    offsetof(align_test,r), /* alignment */
    0,                      /* subarray */
    0,                      /* fields */
    0,                      /* names */
    &npyrational_arrfuncs,  /* f */
};

#define DEFINE_CAST(From,To,statement) \
    static void \
    npycast_##From##_##To(void* from_, void* to_, npy_intp n, void* fromarr, void* toarr) { \
        const From* from = (From*)from_; \
        To* to = (To*)to_; \
        npy_intp i; \
        for (i = 0; i < n; i++) { \
            From x = from[i]; \
            statement \
            to[i] = y; \
        } \
    }
#define DEFINE_INT_CAST(bits) \
    DEFINE_CAST(int##bits##_t,rational,rational y = make_rational_int(x);) \
    DEFINE_CAST(rational,int##bits##_t,int32_t z = rational_int(x); int##bits##_t y = z; if (y != z) set_overflow();)
DEFINE_INT_CAST(8)
DEFINE_INT_CAST(16)
DEFINE_INT_CAST(32)
DEFINE_INT_CAST(64)
DEFINE_CAST(rational,float,double y = rational_double(x);)
DEFINE_CAST(rational,double,double y = rational_double(x);)
DEFINE_CAST(npy_bool,rational,rational y = make_rational_int(x);)
DEFINE_CAST(rational,npy_bool,npy_bool y = rational_nonzero(x);)

#define BINARY_UFUNC(name,intype0,intype1,outtype,exp) \
    void name(char** args, npy_intp* dimensions, npy_intp* steps, void* data) { \
        npy_intp is0 = steps[0], is1 = steps[1], os = steps[2], n = *dimensions; \
        char *i0 = args[0], *i1 = args[1], *o = args[2]; \
        int k; \
        for (k = 0; k < n; k++) { \
            intype0 x = *(intype0*)i0; \
            intype1 y = *(intype1*)i1; \
            *(outtype*)o = exp; \
            i0 += is0; i1 += is1; o += os; \
        } \
    }
#define RATIONAL_BINARY_UFUNC(name,type,exp) BINARY_UFUNC(rational_ufunc_##name,rational,rational,type,exp)
RATIONAL_BINARY_UFUNC(add,rational,rational_add(x,y))
RATIONAL_BINARY_UFUNC(subtract,rational,rational_subtract(x,y))
RATIONAL_BINARY_UFUNC(multiply,rational,rational_multiply(x,y))
RATIONAL_BINARY_UFUNC(divide,rational,rational_divide(x,y))
RATIONAL_BINARY_UFUNC(remainder,rational,rational_remainder(x,y))
RATIONAL_BINARY_UFUNC(floor_divide,rational,make_rational_int(rational_floor(rational_divide(x,y))))
PyUFuncGenericFunction rational_ufunc_true_divide = rational_ufunc_divide;
RATIONAL_BINARY_UFUNC(minimum,rational,rational_lt(x,y)?x:y)
RATIONAL_BINARY_UFUNC(maximum,rational,rational_lt(x,y)?y:x)
RATIONAL_BINARY_UFUNC(equal,npy_bool,rational_eq(x,y))
RATIONAL_BINARY_UFUNC(not_equal,npy_bool,rational_ne(x,y))
RATIONAL_BINARY_UFUNC(less,npy_bool,rational_lt(x,y))
RATIONAL_BINARY_UFUNC(greater,npy_bool,rational_gt(x,y))
RATIONAL_BINARY_UFUNC(less_equal,npy_bool,rational_le(x,y))
RATIONAL_BINARY_UFUNC(greater_equal,npy_bool,rational_ge(x,y))

BINARY_UFUNC(gcd_ufunc,int64_t,int64_t,int64_t,gcd(x,y))
BINARY_UFUNC(lcm_ufunc,int64_t,int64_t,int64_t,lcm(x,y))

#define UNARY_UFUNC(name,type,exp) \
    void rational_ufunc_##name(char** args, npy_intp* dimensions, npy_intp* steps, void* data) { \
        npy_intp is = steps[0], os = steps[1], n = *dimensions; \
        char *i = args[0], *o = args[1]; \
        int k; \
        for (k = 0; k < n; k++) { \
            rational x = *(rational*)i; \
            *(type*)o = exp; \
            i += is; o += os; \
        } \
    }
UNARY_UFUNC(negative,rational,rational_negative(x))
UNARY_UFUNC(absolute,rational,rational_abs(x))
UNARY_UFUNC(floor,rational,make_rational_int(rational_floor(x)))
UNARY_UFUNC(ceil,rational,make_rational_int(rational_ceil(x)))
UNARY_UFUNC(trunc,rational,make_rational_int(x.n/d(x)))
UNARY_UFUNC(square,rational,rational_multiply(x,x))
UNARY_UFUNC(rint,rational,make_rational_int(rational_rint(x)))
UNARY_UFUNC(sign,rational,make_rational_int(rational_sign(x)))
UNARY_UFUNC(reciprocal,rational,rational_inverse(x))
UNARY_UFUNC(numerator,int64_t,x.n)
UNARY_UFUNC(denominator,int64_t,d(x))

void rational_ufunc_inplace_multiply(char** args, npy_intp* dimensions, npy_intp* steps, void* data) {
    npy_intp is0 = steps[0], is1 = steps[1], os = steps[2], n = *dimensions;
    char *i0 = args[0], *i1 = args[1], *o = args[2];
    int k;
    for (k = 0; k < n; k++) {
        rational x = *(rational*)i0;
        rational y = *(rational*)i1;
        *(rational*)i0 = rational_multiply(x, y);
        i0 += is0; i1 += is1; o += os;
    }
}

static NPY_INLINE void
rational_matrix_multiply(char **args, npy_intp *dimensions, npy_intp *steps)
{
    /* pointers to data for input and output arrays */
    char *ip1 = args[0];
    char *ip2 = args[1];
    char *op = args[2];

    /* lengths of core dimensions */
    npy_intp dm = dimensions[0];
    npy_intp dn = dimensions[1];
    npy_intp dp = dimensions[2];

    /* striding over core dimensions */
    npy_intp is1_m = steps[0];
    npy_intp is1_n = steps[1];
    npy_intp is2_n = steps[2];
    npy_intp is2_p = steps[3];
    npy_intp os_m = steps[4];
    npy_intp os_p = steps[5];

    /* core dimensions counters */
    npy_intp m, n, p;

    /* calculate dot product for each row/column vector pair */
    for (m = 0; m < dm; m++) {
        for (p = 0; p < dp; p++) {
            npyrational_dot(ip1, is1_n, ip2, is2_n, op, dn, NULL);

            /* advance to next column of 2nd input array and output array */
            ip2 += is2_p;
            op  +=  os_p;
        }

        /* reset to first column of 2nd input array and output array */
        ip2 -= is2_p * p;
        op -= os_p * p;

        /* advance to next row of 1st input array and output array */
        ip1 += is1_m;
        op += os_m;
    }
}


static void
rational_gufunc_matrix_multiply(char **args, npy_intp *dimensions, npy_intp *steps, void *NPY_UNUSED(func))
{
    /* outer dimensions counter */
    npy_intp N_;

    /* length of flattened outer dimensions */
    npy_intp dN = dimensions[0];

    /* striding over flattened outer dimensions for input and output arrays */
    npy_intp s0 = steps[0];
    npy_intp s1 = steps[1];
    npy_intp s2 = steps[2];

    /* loop through outer dimensions, performing matrix multiply on core dimensions for each loop */
    for (N_ = 0; N_ < dN; N_++, args[0] += s0, args[1] += s1, args[2] += s2) {
        rational_matrix_multiply(args, dimensions+1, steps+3);
    }
}


PyMethodDef module_methods[] = {
    {0} /* sentinel */
};

PyMODINIT_FUNC
initrational(void) {
    /* Initialize numpy */
    import_array();
    if (PyErr_Occurred()) {
        return;
    }
    import_umath();
    if (PyErr_Occurred()) {
        return;
    }
    PyObject* numpy_str = PyString_FromString("numpy");
    if (!numpy_str) {
        return;
    }
    PyObject* numpy = PyImport_Import(numpy_str);
    Py_DECREF(numpy_str);
    if (!numpy) {
        return;
    }

    /* Can't set this until we import numpy */
    PyRational_Type.tp_base = &PyGenericArrType_Type;

    /* Initialize rational type object */
    if (PyType_Ready(&PyRational_Type) < 0) {
        return;
    }

    /* Initialize rational descriptor */
    PyArray_InitArrFuncs(&npyrational_arrfuncs);
    npyrational_arrfuncs.getitem = npyrational_getitem;
    npyrational_arrfuncs.setitem = npyrational_setitem;
    npyrational_arrfuncs.copyswapn = npyrational_copyswapn;
    npyrational_arrfuncs.copyswap = npyrational_copyswap;
    npyrational_arrfuncs.compare = npyrational_compare;
    npyrational_arrfuncs.argmin = npyrational_argmin;
    npyrational_arrfuncs.argmax = npyrational_argmax;
    npyrational_arrfuncs.dotfunc = npyrational_dot;
    npyrational_arrfuncs.nonzero = npyrational_nonzero;
    npyrational_arrfuncs.fill = npyrational_fill;
    npyrational_arrfuncs.fillwithscalar = npyrational_fillwithscalar;
    /* Left undefined: scanfunc, fromstr, sort, argsort */
    npyrational_descr.ob_type = &PyArrayDescr_Type;
    int npy_rational = PyArray_RegisterDataType(&npyrational_descr);
    if (npy_rational<0) {
        return;
    }

    /* Support dtype(rational) syntax */
    if (PyDict_SetItemString(PyRational_Type.tp_dict,"dtype",(PyObject*)&npyrational_descr)<0) {
        return;
    }

    /* Register casts to and from rational */
    #define REGISTER_CAST(From,To,from_descr,to_typenum,safe) \
        PyArray_Descr* from_descr_##From##_##To = (from_descr); \
        if (PyArray_RegisterCastFunc(from_descr_##From##_##To,(to_typenum),npycast_##From##_##To)<0) { \
            return; \
        } \
        if (safe && PyArray_RegisterCanCast(from_descr_##From##_##To,(to_typenum),NPY_NOSCALAR)<0) { \
            return; \
        }
    #define REGISTER_INT_CASTS(bits) \
        REGISTER_CAST(int##bits##_t,rational,PyArray_DescrFromType(NPY_INT##bits),npy_rational,1) \
        REGISTER_CAST(rational,int##bits##_t,&npyrational_descr,NPY_INT##bits,0)
    REGISTER_INT_CASTS(8)
    REGISTER_INT_CASTS(16)
    REGISTER_INT_CASTS(32)
    REGISTER_INT_CASTS(64)
    REGISTER_CAST(rational,float,&npyrational_descr,NPY_FLOAT,0)
    REGISTER_CAST(rational,double,&npyrational_descr,NPY_DOUBLE,1)
    REGISTER_CAST(npy_bool,rational,PyArray_DescrFromType(NPY_BOOL),npy_rational,1)
    REGISTER_CAST(rational,npy_bool,&npyrational_descr,NPY_BOOL,0)

    /* Register ufuncs */
    #define REGISTER_UFUNC(name,...) { \
        PyUFuncObject* ufunc = (PyUFuncObject*)PyObject_GetAttrString(numpy,#name); \
        if (!ufunc) { \
            return; \
        } \
        int _types[] = __VA_ARGS__; \
        if (sizeof(_types)/sizeof(int)!=ufunc->nargs) { \
            PyErr_Format(PyExc_AssertionError,"ufunc %s takes %d arguments, our loop takes %ld",#name,ufunc->nargs,sizeof(_types)/sizeof(int)); \
            return; \
        } \
        if (PyUFunc_RegisterLoopForType((PyUFuncObject*)ufunc,npy_rational,rational_ufunc_##name,_types,0)<0) { \
            return; \
        } \
    }
    #define REGISTER_UFUNC_BINARY_RATIONAL(name) REGISTER_UFUNC(name,{npy_rational,npy_rational,npy_rational})
    #define REGISTER_UFUNC_BINARY_COMPARE(name) REGISTER_UFUNC(name,{npy_rational,npy_rational,NPY_BOOL})
    #define REGISTER_UFUNC_UNARY(name) REGISTER_UFUNC(name,{npy_rational,npy_rational})
    /* Binary */
    REGISTER_UFUNC_BINARY_RATIONAL(add)
    REGISTER_UFUNC_BINARY_RATIONAL(subtract)
    REGISTER_UFUNC_BINARY_RATIONAL(multiply)
    REGISTER_UFUNC_BINARY_RATIONAL(divide)
    REGISTER_UFUNC_BINARY_RATIONAL(remainder)
    REGISTER_UFUNC_BINARY_RATIONAL(true_divide)
    REGISTER_UFUNC_BINARY_RATIONAL(floor_divide)
    REGISTER_UFUNC_BINARY_RATIONAL(minimum)
    REGISTER_UFUNC_BINARY_RATIONAL(maximum)
    /* Comparisons */
    REGISTER_UFUNC_BINARY_COMPARE(equal)
    REGISTER_UFUNC_BINARY_COMPARE(not_equal)
    REGISTER_UFUNC_BINARY_COMPARE(less)
    REGISTER_UFUNC_BINARY_COMPARE(greater)
    REGISTER_UFUNC_BINARY_COMPARE(less_equal)
    REGISTER_UFUNC_BINARY_COMPARE(greater_equal)
    /* Unary */
    REGISTER_UFUNC_UNARY(negative)
    REGISTER_UFUNC_UNARY(absolute)
    REGISTER_UFUNC_UNARY(floor)
    REGISTER_UFUNC_UNARY(ceil)
    REGISTER_UFUNC_UNARY(trunc)
    REGISTER_UFUNC_UNARY(rint)
    REGISTER_UFUNC_UNARY(square)
    REGISTER_UFUNC_UNARY(reciprocal)
    REGISTER_UFUNC_UNARY(sign)

    /* Create module */
    PyObject* m = Py_InitModule3("rational", module_methods,
        "Fixed precision rational numbers, including numpy support");
    if (!m) {
        return;
    }

    /* Add rational type */
    Py_INCREF(&PyRational_Type);
    PyModule_AddObject(m,"rational",(PyObject*)&PyRational_Type);

    /* Create inplace multiply ufunc */
    PyObject* ufunc2 = PyUFunc_FromFuncAndData(0,0,0,0,2,1,PyUFunc_None,(char*)"inplace_multiply",(char*)"inplace multiply for rational dtype",0);
    ((PyUFuncObject*)ufunc2)->op_flags[0] = NPY_ITER_READWRITE;
    ((PyUFuncObject*)ufunc2)->iter_flags = NPY_ITER_REDUCE_OK;
    int ufunc2_types[] = {npy_rational, npy_rational, npy_rational};
    PyUFunc_RegisterLoopForType((PyUFuncObject*)ufunc2,npy_rational,rational_ufunc_inplace_multiply,ufunc2_types,0);
    PyModule_AddObject(m,"inplace_multiply",(PyObject*)ufunc2);

    /* Create matrix multiply generalized ufunc */
    PyObject* gufunc = PyUFunc_FromFuncAndDataAndSignature(0,0,0,0,2,1,PyUFunc_None,(char*)"matrix_multiply",(char*)"return result of multiplying two matrices of rationals",0,"(m,n),(n,p)->(m,p)");
    if (!gufunc) {
        return;
    }
    int types2[3] = {npy_rational,npy_rational,npy_rational};
    if (PyUFunc_RegisterLoopForType((PyUFuncObject*)gufunc,npy_rational,rational_gufunc_matrix_multiply,types2,0) < 0) {
        return;
    }
    PyModule_AddObject(m,"matrix_multiply",(PyObject*)gufunc);

    /* Create numerator and denominator ufuncs */
    #define NEW_UNARY_UFUNC(name,type,doc) { \
        PyObject* ufunc = PyUFunc_FromFuncAndData(0,0,0,0,1,1,PyUFunc_None,(char*)#name,(char*)doc,0); \
        if (!ufunc) { \
            return; \
        } \
        int types[2] = {npy_rational,type}; \
        if (PyUFunc_RegisterLoopForType((PyUFuncObject*)ufunc,npy_rational,rational_ufunc_##name,types,0)<0) { \
            return; \
        } \
        PyModule_AddObject(m,#name,(PyObject*)ufunc); \
    }
    NEW_UNARY_UFUNC(numerator,NPY_INT64,"rational number numerator");
    NEW_UNARY_UFUNC(denominator,NPY_INT64,"rational number denominator");

    /* Create gcd and lcm ufuncs */
    #define GCD_LCM_UFUNC(name,type,doc) { \
        static const PyUFuncGenericFunction func[1] = {name##_ufunc}; \
        static const char types[3] = {type,type,type}; \
        static void* data[1] = {0}; \
        PyObject* ufunc = PyUFunc_FromFuncAndData((PyUFuncGenericFunction*)func,data,(char*)types,1,2,1,PyUFunc_One,(char*)#name,(char*)doc,0); \
        if (!ufunc) { \
            return; \
        } \
        PyModule_AddObject(m,#name,(PyObject*)ufunc); \
    }
    GCD_LCM_UFUNC(gcd,NPY_INT64,"greatest common denominator of two integers");
    GCD_LCM_UFUNC(lcm,NPY_INT64,"least common multiple of two integers");
}
