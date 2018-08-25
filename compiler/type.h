#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <onnx/onnx.pb.h>

#include <compiler/dtype.h>

namespace oniku {

class Type {
public:
    explicit Type(const onnx::TypeProto& xtype);
    Type(Dtype dtype, const std::vector<int64_t>& dims);

    void ToONNX(onnx::TypeProto* xtype) const;

    Dtype dtype() const {
        return dtype_;
    }

    const std::vector<int64_t>& dims() const {
        return dims_;
    }

    int64_t NumElements() const;

private:
    Dtype dtype_;
    std::vector<int64_t> dims_;
    std::vector<std::string> dim_params_;
    std::vector<std::string> denotations_;
};

}  // namespace oniku
