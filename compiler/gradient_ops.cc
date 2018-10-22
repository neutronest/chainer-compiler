#include "gradient_ops.h"

#include <atomic>
#include <map>
#include <memory>
#include <string>

#include <common/log.h>
#include <common/strutil.h>
#include <compiler/gradient.h>
#include <compiler/graph.h>
#include <compiler/graph_builder.h>
#include <compiler/node.h>
#include <compiler/tensor.h>
#include <compiler/type.h>

namespace oniku {
namespace {

class GradientOpContext {
public:
    GradientOpContext(Graph* graph, Node* node, const std::vector<Value*>& x, const std::vector<Value*>& y, bool retain_in_stack)
        : graph_(graph), node_(node), x_(x), y_(y), retain_in_stack_(retain_in_stack) {
        name_ = Node::OpTypeToString(node->op_type());
        const std::string prefix = "Onikux";
        if (HasPrefix(name_, prefix)) name_ = name_.substr(prefix.size());
        name_ += "Grad";
    }

    Graph* graph() {
        return graph_;
    }

    Node* node() {
        return node_;
    }

    Value* Retain(Value* v) {
        if (!retain_in_stack_) return v;
        int id = ++id_;
        GraphBuilder gb(graph_, StrCat(name_, "Retain", id), v);
        gb.MOp(Node::kOnikuxBackpropStackPush, {v}, {})->set_id(id);
        Value* retained = gb.Op(Node::kOnikuxBackpropStackPop, {});
        retained->set_type(new Type(v->type()));
        retained->producer()->set_id(id);
        return retained;
    }

    Value* x(int i) {
        CHECK_LE(0, i) << i;
        CHECK_GT(x_.size(), i) << i;
        return Retain(x_[i]);
    }

    Value* y(int i) {
        CHECK_LE(0, i) << i;
        CHECK_GT(y_.size(), i) << i;
        return Retain(y_[i]);
    }

    Value* gy(int i) {
        CHECK_LE(0, i) << i;
        CHECK_GT(y_.size(), i) << i;
        return y_[i]->grad();
    }

    GraphBuilder builder(int xi) {
        return GraphBuilder(graph_, name_, x(xi));
    }

    void SetGrad(int xi, Value* gx) {
        CHECK_LE(0, xi) << xi;
        CHECK_GT(x_.size(), xi) << xi;
        Value* x = x_[xi];
        if (x->grad()) {
            // Accumulate gradients.
            GraphBuilder gb(graph_, "AccumGrad", x->grad());
            Value* v = gb.Op(Node::kAdd, {x->grad(), gx});
            x->set_grad(v);
        } else {
            x->set_grad(gx);
        }
    }

    Value* AddGradValue(int xi) {
        Value* gv = graph_->AddValue("grad@" + x(xi)->name());
        SetGrad(xi, gv);
        return gv;
    }

    Value* GradOp(Node::OpType op_type, int xi, const std::vector<Value*>& inputs) {
        Value* gv = AddGradValue(xi);
        graph_->AddNode(op_type, inputs, {gv}, name_);
        return gv;
    }

private:
    Graph* graph_;
    Node* node_;
    const std::vector<Value*>& x_;
    const std::vector<Value*>& y_;
    std::string name_;
    bool retain_in_stack_;
    static std::atomic<int> id_;
};

std::atomic<int> GradientOpContext::id_;

void AddGradFn(GradientOpContext* gc) {
    gc->SetGrad(0, gc->gy(0));
    gc->SetGrad(1, gc->gy(0));
}

void SubGradFn(GradientOpContext* gc) {
    gc->SetGrad(0, gc->gy(0));
    gc->GradOp(Node::kNeg, 1, {gc->gy(0)});
}

void MulGradFn(GradientOpContext* gc) {
    gc->GradOp(Node::kMul, 0, {gc->x(1), gc->gy(0)});
    gc->GradOp(Node::kMul, 1, {gc->x(0), gc->gy(0)});
}

void DivGradFn(GradientOpContext* gc) {
    Value* gy = gc->gy(0);
    Value* gx0 = gc->GradOp(Node::kDiv, 0, {gy, gc->x(1)});

    GraphBuilder gb{gc->builder(1)};
    Value* t0 = gb.Op(Node::kNeg, {gx0});
    Value* t1 = gb.Op(Node::kMul, {t0, gc->x(0)});
    gc->GradOp(Node::kDiv, 1, {t1, gc->x(1)});
}

void NegGradFn(GradientOpContext* gc) {
    gc->GradOp(Node::kNeg, 0, {gc->gy(0)});
}

void ExpGradFn(GradientOpContext* gc) {
    gc->GradOp(Node::kMul, 0, {gc->y(0), gc->gy(0)});
}

void SigmoidGradFn(GradientOpContext* gc) {
    // TODO(hamaji): Support non-float values.
    CHECK_EQ(Dtype::kFloat32, gc->x(0)->type().dtype());
    GraphBuilder gb{gc->builder(0)};
    Value* gy = gc->gy(0);
    Value* one = gb.Const(Type(gc->x(0)->type().dtype(), {}), {1.0});
    Value* t0 = gb.Op(Node::kMul, {gy, gc->y(0)});
    Value* t1 = gb.Op(Node::kSub, {one, gc->y(0)});
    gc->GradOp(Node::kMul, 0, {t0, t1});
}

void ReluGradFn(GradientOpContext* gc) {
    gc->GradOp(Node::kOnikuxReluGrad, 0, {gc->x(0), gc->gy(0)});
}

void SqrtGradFn(GradientOpContext* gc) {
    GraphBuilder gb{gc->builder(0)};
    Value* t0 = gb.Op(Node::kAdd, {gc->y(0), gc->y(0)});
    gc->GradOp(Node::kDiv, 0, {gc->gy(0), t0});
}

void TanhGradFn(GradientOpContext* gc) {
    GraphBuilder gb{gc->builder(0)};
    Value* one = gb.Const(Type(gc->x(0)->type().dtype(), {}), {1.0});
    Value* gy = gc->gy(0);
    Value* t0 = gb.Op(Node::kMul, {gc->y(0), gc->y(0)});
    Value* t1 = gb.Op(Node::kSub, {one, t0});
    gc->GradOp(Node::kMul, 0, {gy, t1});
}

void IdentityGradFn(GradientOpContext* gc) {
    gc->GradOp(Node::kIdentity, 0, {gc->gy(0)});
}

void ReshapeGradFn(GradientOpContext* gc) {
    GraphBuilder gb{gc->builder(0)};
    Value* t0 = gb.Op(Node::kShape, {gc->x(0)});
    gc->GradOp(Node::kReshape, 0, {gc->gy(0), t0});
}

void SelectItemGradFn(GradientOpContext* gc) {
    GraphBuilder gb{gc->builder(0)};
    Value* t0 = gb.Op(Node::kShape, {gc->x(0)});
    gc->GradOp(Node::kOnikuxSelectItemGrad, 0, {gc->gy(0), gc->x(1), t0});
}

void ReduceSumGradFn(GradientOpContext* gc) {
    GraphBuilder gb{gc->builder(0)};
    // TODO(hamaji): Need some check for `axes` and `keepdims`.
    Value* gy = gc->gy(0);
    Value* shape = gb.Op(Node::kShape, {gc->x(0)});
    gc->GradOp(Node::kExpand, 0, {gy, shape});
}

void ReduceMeanGradFn(GradientOpContext* gc) {
    GraphBuilder gb{gc->builder(0)};
    // TODO(hamaji): Need some check for `axes` and `keepdims`.
    Value* gy = gc->gy(0);
    Value* shape = gb.Op(Node::kShape, {gc->x(0)});
    Value* zero = gb.Const(Type(Dtype::kInt64, {}), {0});
    zero->producer()->set_onikux_host(true);
    Value* batch_size_int = gb.Op(Node::kGather, {shape, zero});
    Value* batch_size = gb.Op(Node::kCast, {batch_size_int});
    batch_size->producer()->set_to(Dtype::kFloat32);
    Value* divided = gb.Op(Node::kDiv, {gy, batch_size});
    gc->GradOp(Node::kExpand, 0, {divided, shape});
}

void GemmGradFn(GradientOpContext* gc) {
    const Node* node = gc->node();
    // TODO(hamaji): I'm not sure this function is right. I mean I'm
    // pretty sure something is wrong.
    Value* gy = gc->gy(0);

    // Note bias will be ignored thanks to beta=0.
    {
        GraphBuilder gb{gc->builder(0)};
        Value* gx0 = nullptr;
        if (node->trans_a()) {
            gx0 = gb.Op(Node::kGemm, {gc->x(1), gy, gc->x(0)});
            gx0->producer()->set_alpha(node->alpha())->set_beta(0)->set_trans_a(node->trans_b())->set_trans_b(true);
        } else {
            gx0 = gb.Op(Node::kGemm, {gy, gc->x(1), gc->x(0)});
            gx0->producer()->set_alpha(node->alpha())->set_beta(0)->set_trans_a(false)->set_trans_b(!node->trans_b());
        }
        Value* shape0 = gb.Op(Node::kShape, {gc->x(0)});
        gc->GradOp(Node::kReshape, 0, {gx0, shape0});
    }

    {
        GraphBuilder gb{gc->builder(1)};
        Value* gx1 = nullptr;
        if (node->trans_b()) {
            gx1 = gb.Op(Node::kGemm, {gy, gc->x(0), gc->x(1)});
            gx1->producer()->set_alpha(node->alpha())->set_beta(0)->set_trans_a(true)->set_trans_b(node->trans_a());
        } else {
            gx1 = gb.Op(Node::kGemm, {gc->x(0), gy, gc->x(1)});
            gx1->producer()->set_alpha(node->alpha())->set_beta(0)->set_trans_a(!node->trans_a())->set_trans_b(false);
        }
        Value* shape1 = gb.Op(Node::kShape, {gc->x(1)});
        gc->GradOp(Node::kReshape, 1, {gx1, shape1});
    }

    gc->GradOp(Node::kReduceSum, 2, {gy})->producer()->set_axes({0})->set_keepdims(false);
}

void ConvGradFn(GradientOpContext* gc) {
    const Node* node = gc->node();
    Value* gy = gc->gy(0);
    Value* w = gc->x(1);
    // TODO(hamaji): Revisit how we handle shapes.
#if 0
    gc->GradOp(Node::kConvTranspose, 0, {gy, w})->producer()
        ->set_strides(node->strides())->set_pads(node->pads());
#else
    {
        GraphBuilder gb{gc->builder(0)};
        Value* x_shape = gb.Op(Node::kShape, {gc->x(0)});
        gc->GradOp(Node::kOnikuxConvTransposeWithDynamicOutputShape, 0, {gy, w, x_shape})
                ->producer()
                ->set_strides(node->strides())
                ->set_pads(node->pads());
    }
#endif
    gc->GradOp(Node::kOnikuxConvGradWeight, 1, {w, gc->x(0), gy})->producer()->set_strides(node->strides())->set_pads(node->pads());
    if (node->inputs().size() == 3) {
        std::vector<int> axes{{0}};
        CHECK(!node->kernel_shape().empty()) << "ConvGrad with no kernel_shape is not supported yet.";
        for (size_t i = 0; i < node->kernel_shape().size(); ++i) {
            axes.push_back(2 + i);
        }
        gc->GradOp(Node::kReduceSum, 2, {gy})->producer()->set_axes(axes)->set_keepdims(false);
    }
}

void MaxPoolGradFn(GradientOpContext* gc) {
    gc->GradOp(Node::kOnikuxMaxPoolGrad, 0, {gc->y(0), gc->gy(0)});
}

void AveragePoolGradFn(GradientOpContext* gc) {
    gc->GradOp(Node::kOnikuxAveragePoolGrad, 0, {gc->y(0), gc->gy(0)});
}

void LogSoftmaxGradFn(GradientOpContext* gc) {
    const Node* node = gc->node();
    GraphBuilder gb{gc->builder(0)};
    // TODO(hamaji): This probably works as is. Test it.
    CHECK_EQ(1, node->axis());

    Value* gy = gc->gy(0);
    Value* sum_val = gb.Op(Node::kReduceSum, {gy});
    sum_val->producer()->set_axes({node->axis()})->set_keepdims(true);
    Value* exp_val = gb.Op(Node::kExp, {gc->y(0)});
    Value* mul_val = gb.Op(Node::kMul, {exp_val, sum_val});
    gc->GradOp(Node::kSub, 0, {gy, mul_val});
}

void SoftmaxGradFn(GradientOpContext* gc) {
    const Node* node = gc->node();
    GraphBuilder gb{gc->builder(0)};
    Value* gy = gc->gy(0);
    Value* gx = gb.Op(Node::kMul, {gc->y(0), gy});
    Value* sum_val = gb.Op(Node::kReduceSum, {gx});
    sum_val->producer()->set_axes({node->axis()})->set_keepdims(true);
    Value* mul_val = gb.Op(Node::kMul, {gc->y(0), sum_val});
    gc->GradOp(Node::kSub, 0, {gx, mul_val});
}

void BatchNormalizationGradFn(GradientOpContext* gc) {
    Value* gx0 = gc->AddGradValue(0);
    Value* gx1 = gc->AddGradValue(1);
    Value* gx2 = gc->AddGradValue(2);
    gc->graph()->AddNode(Node::kOnikuxBatchNormalizationGrad, {gc->y(0), gc->gy(0)}, {gx0, gx1, gx2}, __func__);
    Value* zero = gc->graph()->AddConstValue("grad_tmp_zero@" + gc->x(0)->name(), Type(gc->x(0)->type().dtype(), {1}), {0.0});
    // No gradients since update should have been done for running mean/variance.
    gc->SetGrad(3, zero);
    gc->SetGrad(4, zero);
}

void LRNGradFn(GradientOpContext* gc) {
    const Node* node = gc->node();
    gc->GradOp(Node::kOnikuxLRNGrad, 0, {gc->x(0), gc->y(0), gc->gy(0)})
            ->producer()
            ->set_alpha(node->alpha())
            ->set_beta(node->beta())
            ->set_bias(node->bias())
            ->set_size(node->size());
}

void DoNothingGradFn(GradientOpContext*) {
}

void OutputIterationCount(Graph* graph, Node* loop) {
    int num_states = loop->inputs().size() - 2;

    {
        GraphBuilder gb(graph, "LoopGradIterCnt", loop->outputs()[0]);
        Value* input_iter = gb.Const(Type(Dtype::kInt64, {}), {0});
        loop->AddInput(input_iter);
        Value* output_iter = graph->AddValue(gb.GenName());
        loop->AddOutput(output_iter, num_states);
    }

    {
        Graph* body = loop->body().get();
        GraphBuilder gb(body, "LoopGradIterCntBody", loop->outputs()[0]);
        Value* one = gb.Const(Type(Dtype::kInt64, {}), {1});
        Value* input_cnt = new Value(gb.GenName(), Type(Dtype::kInt64, {}), Value::Kind::kInput);
        Value* output_cnt = new Value(gb.GenName(), Type(Dtype::kInt64, {}), Value::Kind::kOutput);
        gb.Op(Node::kAdd, {input_cnt, one}, {output_cnt});
        body->mutable_input_values()->push_back(input_cnt);
        body->mutable_output_values()->push_back(output_cnt);
    }
}

void LoopGradFn(GradientOpContext* gc) {
    Graph* graph = gc->graph();
    Node* loop = gc->node();
    OutputIterationCount(graph, loop);
    const std::vector<Value*>& xs = loop->inputs();
    const std::vector<Value*>& ys = loop->outputs();
    Graph* body = loop->body().get();
    int num_loop_inputs = xs.size();
    int num_loop_outputs = ys.size();
    int num_body_inputs = body->input_values().size();
    int num_body_outputs = body->output_values().size();
    int num_states = num_loop_inputs - 2;
    int num_scans = num_body_outputs - 1 - num_states;
    CHECK_EQ(num_body_inputs, num_states + 2);
    CHECK_EQ(num_loop_outputs, num_states + num_scans);

    CHECK_EQ(0, num_scans) << "Not implemented yet";
    CHECK_EQ(0, loop->onikux_stack_axis()) << "Not implemented yet";

    std::vector<std::string> input_value_names;
    std::vector<std::string> output_value_names;
    {
        GraphBuilder gb(body, "LoopGradBody", xs[0]);
        // Two extra inputs for iterator and condition.
        for (int i = 0; i < 2; ++i) {
            input_value_names.push_back(body->AddValue(gb.GenName())->name());
        }
        std::vector<Value*> ys;
        for (int i = 0; i < num_states - 1; ++i) {
            Value* y = body->output_values()[i + 1];
            Value* gy = body->AddValue("loop_grad_in@" + y->name());
            CHECK(y->grad() == nullptr);
            y->set_grad(gb.Op(Node::kIdentity, {gy}));
            ys.push_back(y);
            input_value_names.push_back(gy->name());
        }
        AddGradientNodes(body, ys, true /* retain_in_stack */);

        Value* output_cond = gb.Const(Type(Dtype::kBool, {}), {1});
        output_value_names.push_back(output_cond->name());
        for (int i = 0; i < num_states - 1; ++i) {
            Value* x = body->input_values()[i + 2];
            CHECK(x->grad());
            Value* out = gb.Op(Node::kIdentity, {x->grad()});
            output_value_names.push_back(out->name());
        }
    }

    {
        GraphBuilder gb(graph, "LoopGrad", xs[0]);
        std::vector<Value*> gys;
        for (int i = 0; i < num_states - 1; ++i) {
            Value* y = ys[i];
            CHECK(y->grad());
            gys.push_back(y->grad());
        }
        std::vector<Value*> gxs;
        for (int i = 0; i < num_states - 1; ++i) {
            CHECK(body->input_values()[i + 2]->grad());
            gxs.push_back(gc->AddGradValue(i + 2));
        }

        std::vector<Value*> backward_inputs;
        backward_inputs.push_back(ys[num_states - 1]);
        backward_inputs.push_back(graph->AddValue("", Value::Kind::kNull));
        for (Value* gy : gys) backward_inputs.push_back(gy);

        Node* backward_loop = gb.MOp(Node::kOnikuxLoopRef, backward_inputs, gxs);
        CHECK(!body->name().empty()) << "Loop body must have a name";
        backward_loop->set_body_ref(body->name());
        backward_loop->set_input_value_names(input_value_names);
        backward_loop->set_output_value_names(output_value_names);
    }

    body->ResetGradients();
}

void SequenceStackGradFn(GradientOpContext* gc) {
    const Node* node = gc->node();
    Value* gy = gc->gy(0);
    gc->GradOp(Node::kOnikuxSequenceSplit, 0, {gy})->producer()->set_axis(node->axis());
}

void SequenceAppendGradFn(GradientOpContext* gc) {
    GraphBuilder gb{gc->builder(0)};
    std::vector<Value*> gxs;
    for (int i = 0; i < 2; ++i) {
        gxs.push_back(gc->AddGradValue(i));
    }
    gb.MOp(Node::kOnikuxSequencePop, {gc->gy(0)}, gxs);
}

typedef void (*GradFn)(GradientOpContext*);

struct GradientFunc {
    int num_inputs;
    int num_outputs;
    GradFn fn;
};

}  // namespace

void AddGradientForNode(Graph* graph, Node* node, bool retain_in_stack) {
    static std::map<Node::OpType, GradientFunc>* s_gradient_funcs;
    if (!s_gradient_funcs) {
        // Leak.
        s_gradient_funcs = new std::map<Node::OpType, GradientFunc>;
        auto register_grad_fn = [](Node::OpType op_type, int num_inputs, int num_outputs, GradFn fn) {
            GradientFunc func;
            func.num_inputs = num_inputs;
            func.num_outputs = num_outputs;
            func.fn = fn;
            CHECK(s_gradient_funcs->emplace(op_type, func).second);
        };

        register_grad_fn(Node::kAdd, 2, 1, &AddGradFn);
        register_grad_fn(Node::kSub, 2, 1, &SubGradFn);
        register_grad_fn(Node::kMul, 2, 1, &MulGradFn);
        register_grad_fn(Node::kDiv, 2, 1, &DivGradFn);
        register_grad_fn(Node::kNeg, 1, 1, &NegGradFn);
        register_grad_fn(Node::kExp, 1, 1, &ExpGradFn);
        register_grad_fn(Node::kSigmoid, 1, 1, &SigmoidGradFn);
        register_grad_fn(Node::kRelu, 1, 1, &ReluGradFn);
        register_grad_fn(Node::kSqrt, 1, 1, &SqrtGradFn);
        register_grad_fn(Node::kTanh, 1, 1, &TanhGradFn);

        register_grad_fn(Node::kIdentity, 1, 1, &IdentityGradFn);
        register_grad_fn(Node::kReshape, 2, 1, &ReshapeGradFn);
        register_grad_fn(Node::kOnikuxSelectItem, 2, 1, &SelectItemGradFn);

        register_grad_fn(Node::kReduceSum, 1, 1, &ReduceSumGradFn);
        register_grad_fn(Node::kReduceMean, 1, 1, &ReduceMeanGradFn);
        register_grad_fn(Node::kGemm, 3, 1, &GemmGradFn);
        register_grad_fn(Node::kConv, -1, 1, &ConvGradFn);
        register_grad_fn(Node::kMaxPool, 1, 1, &MaxPoolGradFn);
        register_grad_fn(Node::kAveragePool, 1, 1, &AveragePoolGradFn);
        register_grad_fn(Node::kLogSoftmax, 1, 1, &LogSoftmaxGradFn);
        register_grad_fn(Node::kSoftmax, 1, 1, &SoftmaxGradFn);

        register_grad_fn(Node::kBatchNormalization, 5, -1, &BatchNormalizationGradFn);
        register_grad_fn(Node::kLRN, 1, 1, &LRNGradFn);

        // TODO(hamaji): Implement dropout.
        register_grad_fn(Node::kDropout, 1, 1, &IdentityGradFn);

        register_grad_fn(Node::kGreater, 2, 1, &DoNothingGradFn);
        register_grad_fn(Node::kConstant, 0, 1, &DoNothingGradFn);

        register_grad_fn(Node::kLoop, -1, -1, &LoopGradFn);

        register_grad_fn(Node::kOnikuxSequenceStack, 1, 1, &SequenceStackGradFn);
        register_grad_fn(Node::kOnikuxSequenceAppend, 2, 1, &SequenceAppendGradFn);
    }

    auto found = s_gradient_funcs->find(node->op_type());
    CHECK(found != s_gradient_funcs->end()) << "Gradient not supported: " << node->op_type();
    const GradientFunc& func = found->second;
    if (func.num_inputs >= 0) CHECK_EQ(static_cast<size_t>(func.num_inputs), node->inputs().size());
    if (func.num_outputs >= 0) CHECK_EQ(static_cast<size_t>(func.num_outputs), node->outputs().size());

    GradientOpContext gc(graph, node, node->inputs(), node->outputs(), retain_in_stack);
    func.fn(&gc);
}

}  // namespace oniku
