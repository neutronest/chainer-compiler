#include <chainerx/routines/connection.h>
#include <chainerx/routines/creation.h>
#include <chainerx/routines/linalg.h>
#include <chainerx/routines/logic.h>
#include <chainerx/routines/manipulation.h>
#include <chainerx/routines/math.h>

#include <common/log.h>
#include <runtime/chainerx_util.h>
#include <runtime/gen_xcvm_ops.h>

namespace chainer_compiler {
namespace runtime {

namespace {

chainerx::Array Pow(chainerx::Array a, chainerx::Array b) {
    return chainerx::Exp(chainerx::Log(a) * b);
}

// TODO(hamaji): Implement type coersion in ChainerX.
chainerx::Dtype CoerceDtype(chainerx::Dtype dtype0, chainerx::Dtype dtype1) {
    if (dtype0 == dtype1) return dtype0;
    if (IsFloat(dtype0) && !IsFloat(dtype1)) return dtype0;
    if (!IsFloat(dtype0) && IsFloat(dtype1)) return dtype1;
    if (chainerx::GetItemSize(dtype0) > chainerx::GetItemSize(dtype1)) return dtype0;
    if (chainerx::GetItemSize(dtype0) < chainerx::GetItemSize(dtype1)) return dtype1;
    if (dtype1 == chainerx::Dtype::kBool) return dtype0;
    if (dtype0 == chainerx::Dtype::kBool) return dtype1;
    if (dtype0 == chainerx::Dtype::kUInt8 || dtype1 == chainerx::Dtype::kUInt8) return chainerx::Dtype::kInt16;
    CHECK(false) << "Unknown type coerce: " << dtype0 << " vs " << dtype1;
}

std::tuple<chainerx::Array, chainerx::Array> CoerceBinary(const chainerx::Array& a, const chainerx::Array& b) {
    chainerx::Array ax = a;
    chainerx::Array bx = b;
    chainerx::Dtype dtype = CoerceDtype(a.dtype(), b.dtype());
    ax = CastTo(ax, dtype);
    bx = CastTo(bx, dtype);
    return std::tie(ax, bx);
}

}  // namespace

chainerx::Array AddOp::RunImpl(XCVMState* st, const chainerx::Array& a, const chainerx::Array& b) {
    auto t = CoerceBinary(a, b);
    return std::get<0>(t) + std::get<1>(t);
}

chainerx::Array SubOp::RunImpl(XCVMState* st, const chainerx::Array& a, const chainerx::Array& b) {
    auto t = CoerceBinary(a, b);
    return std::get<0>(t) - std::get<1>(t);
}

chainerx::Array MulOp::RunImpl(XCVMState* st, const chainerx::Array& a, const chainerx::Array& b) {
    auto t = CoerceBinary(a, b);
    return std::get<0>(t) * std::get<1>(t);
}

chainerx::Array DivOp::RunImpl(XCVMState* st, const chainerx::Array& a0, const chainerx::Array& b0) {
    chainerx::Array a, b;
    std::tie(a, b) = CoerceBinary(a0, b0);
    // TODO(hamaji): Come up with a better idea to handle cross device ops.
    if (&a.device() != &b.device() && b.GetTotalSize() == 1) {
        if (IsFloat(a.dtype())) {
            return chainerx::TrueDivide(a, chainerx::AsScalar(b));
        } else {
            return chainerx::FloorDivide(a, chainerx::AsScalar(b));
        }
    }
    if (IsFloat(a.dtype())) {
        return chainerx::TrueDivide(a, b);
    } else {
        return chainerx::FloorDivide(a, b);
    }
}

chainerx::Array PowOp::RunImpl(XCVMState* st, const chainerx::Array& a, const chainerx::Array& b) {
    auto t = CoerceBinary(a, b);
    return Pow(std::get<0>(t), std::get<1>(t));
}

chainerx::Array NegOp::RunImpl(XCVMState* st, const chainerx::Array& a) {
    return -a;
}

#define DEFINE_UNARY_OP(op)                                                    \
    chainerx::Array op##Op::RunImpl(XCVMState* st, const chainerx::Array& a) { \
        return chainerx::op(a);                                                \
    }

#define DEFINE_UNARY_OP_TODO(op)                                               \
    chainerx::Array op##Op::RunImpl(XCVMState* st, const chainerx::Array& a) { \
        CHECK(false) << "TODO(hamaji): " #op " op not implemented";            \
    }

DEFINE_UNARY_OP(Exp);
DEFINE_UNARY_OP(Log);
DEFINE_UNARY_OP(Sqrt);
DEFINE_UNARY_OP(Reciprocal);
DEFINE_UNARY_OP(Sin);
DEFINE_UNARY_OP(Cos);
DEFINE_UNARY_OP(Tan);
DEFINE_UNARY_OP(Arcsin);
DEFINE_UNARY_OP(Arccos);
DEFINE_UNARY_OP(Arctan);
DEFINE_UNARY_OP_TODO(Sinh);
DEFINE_UNARY_OP_TODO(Cosh);
DEFINE_UNARY_OP_TODO(Arcsinh);
DEFINE_UNARY_OP_TODO(Arccosh);
DEFINE_UNARY_OP_TODO(Arctanh);

chainerx::Array AbsOp::RunImpl(XCVMState* st, const chainerx::Array& x) {
    chainerx::Array negs = (x < chainerx::Zeros({}, x.dtype(), x.device())).AsType(x.dtype());
    return x * (1 - negs * 2);
}

chainerx::Array FloorOp::RunImpl(XCVMState* st, const chainerx::Array& x) {
    if (!IsFloat(x.dtype())) {
        return x;
    }
    return chainerx::Floor(x);
}

chainerx::Array CeilOp::RunImpl(XCVMState* st, const chainerx::Array& x) {
    if (!IsFloat(x.dtype())) {
        return x;
    }
    return chainerx::Ceil(x);
}

chainerx::Array ClipOp::RunImpl(XCVMState* st, const chainerx::Array& x) {
    return -chainerx::Maximum(-chainerx::Maximum(x, min), -max);
}

chainerx::Array MatMulOp::RunImpl(XCVMState* st, const chainerx::Array& a, const chainerx::Array& b) {
    // TODO(hamaji): Handle non 2D arrays.
    return chainerx::Dot(a, b);
}

chainerx::Array GemmOp::RunImpl(XCVMState* st, const chainerx::Array& a, const chainerx::Array& b, const chainerx::Array& c) {
    if (alpha == 1.0 && beta == 1.0 && !trans_a && trans_b && c.ndim() == 1) {
        return Linear(a, b, c);
    }

    chainerx::Array xa = a;
    chainerx::Array xb = b;
    if (trans_a) xa = chainerx::Transpose(xa);
    if (trans_b) xb = chainerx::Transpose(xb);
    chainerx::Array r = chainerx::Dot(xa, xb);
    if (alpha != 1.0) r *= alpha;
    if (beta == 0.0) return r;
    chainerx::Array xc = c;
    if (beta != 1.0) xc = xc * beta;
    return r + xc;
}

chainerx::Array MaxOp::RunImpl(XCVMState* st, const std::vector<chainerx::Array>& inputs) {
    CHECK_LT(0, inputs.size());
    chainerx::Array result = inputs[0];
    for (size_t i = 1; i < inputs.size(); ++i) {
        result = Maximum(result, inputs[i]);
    }
    return result;
}

}  // namespace runtime
}  // namespace chainer_compiler
