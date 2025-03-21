#include <test/cpp/jit/test_utils.h>

#include <gtest/gtest.h>

#include <c10/core/TensorOptions.h>
#include <torch/csrc/autograd/generated/variable_factories.h>
#include <torch/csrc/jit/api/module.h>
#include <torch/csrc/jit/frontend/resolver.h>
#include <torch/csrc/jit/mobile/backport.h>
#include <torch/csrc/jit/mobile/backport_manager.h>
#include <torch/csrc/jit/mobile/flatbuffer_loader.h>
#include <torch/csrc/jit/mobile/import.h>
#include <torch/csrc/jit/mobile/interpreter.h>
#include <torch/csrc/jit/mobile/model_compatibility.h>
#include <torch/csrc/jit/mobile/module.h>
#include <torch/csrc/jit/mobile/parse_bytecode.h>
#include <torch/csrc/jit/mobile/parse_operators.h>
#include <torch/csrc/jit/mobile/runtime_compatibility.h>
#include <torch/csrc/jit/serialization/export.h>
#include <torch/csrc/jit/serialization/flatbuffer_serializer.h>
#include <torch/csrc/jit/serialization/import.h>
#include <torch/custom_class.h>
#include <torch/torch.h>

#include <torch/csrc/jit/serialization/import_export_functions.h>
#include <unordered_set>
// Tests go in torch::jit
namespace torch {
namespace jit {

mobile::Module parse_mobile_module(void* data, size_t) {
  auto* flatbuffer_module = mobile::serialization::GetMutableModule(data);
  return initialize_mobile_module(flatbuffer_module);
}

TEST(LiteInterpreterTest, UpsampleNearest2d) {
  Module m("m");
  m.define(R"(
    def forward(self, input: Tensor, scale:float):
      return torch.upsample_nearest2d(input, [1, 1], float(scale), float(scale))
  )");

  std::vector<IValue> inputs;
  inputs.emplace_back(torch::rand({1, 3, 128, 128}));
  inputs.emplace_back(at::Scalar(2.0));
  auto ref = m.forward(inputs);

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  IValue res;
  res = bc.forward(inputs);

  auto resd = res.toTensor();
  auto refd = ref.toTensor();
  ASSERT_TRUE(resd.equal(refd));

  auto buff = save_mobile_module_to_bytes(bc);
  mobile::Module bc2 = parse_mobile_module(buff.data(), buff.size());
  auto res2 = bc2.forward(inputs);
  auto resd2 = res2.toTensor();
  ASSERT_TRUE(resd2.equal(refd));
}

TEST(LiteInterpreterTest, CheckAttrAccess) {
  Module m("m");
  m.register_attribute("mobile_optimized", BoolType::get(), true);

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  bool mobile_optimized = bc.attr("mobile_optimized", false).toBool();

  AT_ASSERT(mobile_optimized);
  m.setattr("mobile_optimized", false);
  ss = std::stringstream();
  m._save_for_mobile(ss);
  bc = _load_for_mobile(ss);
  mobile_optimized = bc.attr("mobile_optimized", false).toBool();

  AT_ASSERT(!mobile_optimized);

  auto buff = save_mobile_module_to_bytes(bc);
  mobile::Module bc2 = parse_mobile_module(buff.data(), buff.size());
  auto mobile_optimized2 = bc2.attr("mobile_optimized", false).toBool();
  AT_ASSERT(!mobile_optimized2);
}

TEST(LiteInterpreterTest, MethodInvocation) { // NOLINT (use =delete in gtest)
  const std::vector<std::string> test_programs{
      // test invoking a method with default parameter
      R"(
      def test_func(self, x, b : int = 4):
        return self.foo + x + b
      )",
      // inner method call with default parameter (gets inlined)
      R"(
      def add_with_default_arg(self, x, b : int = 4):
        return self.foo + x + b
      def test_func(self, x):
        return self.add_with_default_arg(x)  # invoke method w/ default arg
      )",
      // simple method call
      R"(
      def test_func(self, x):
        b = 4
        return self.foo + x + b
      )",
  };
  for (const auto& test_program : test_programs) {
    Module m("m");
    m.register_parameter("foo", torch::ones({}), false);
    m.define(test_program);

    const int fortyTwo = 42; // (keep linter happy)
    auto minput = fortyTwo * torch::ones({});
    auto ref = m.run_method("test_func", minput);

    std::stringstream ss;
    m._save_for_mobile(ss);
    mobile::Module bc = _load_for_mobile(ss);
    const auto& test_func = bc.get_method("test_func");
    IValue res;
    for (int i = 0; i < 3; ++i) {
      res = test_func({minput});
    }

    auto resd = res.toTensor().item<float>();
    auto refd = ref.toTensor().item<float>();
    AT_ASSERT(resd == refd);

    auto buff = save_mobile_module_to_bytes(bc);
    mobile::Module bc2 = parse_mobile_module(buff.data(), buff.size());
    const auto& test_func2 = bc2.get_method("test_func");
    IValue res2;
    for (int i = 0; i < 3; ++i) {
      res2 = test_func2({minput});
    }
    auto resd2 = res2.toTensor().item<float>();
    AT_ASSERT(resd2 == refd);
  }
}

TEST(LiteInterpreterTest, Conv) {
  auto s = std::getenv("PYTORCH_TEST_WITH_TSAN");
  if (s && strcmp(s, "1") == 0)
    return;

  std::vector<torch::jit::IValue> inputs;

  Module m("m");
  m.register_parameter("weight", torch::ones({20, 1, 5, 5}), false);
  m.register_parameter("bias", torch::ones({20}), false);
  m.define(R"(
    def forward(self, input):
      return torch._convolution(input, self.weight, self.bias, [1, 1], [0, 0], [1, 1], False, [0, 0], 1, False, False, True, True)
  )");

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,modernize-use-emplace)
  inputs.push_back(torch::ones({1, 1, 28, 28}));

  auto outputref = m.forward(inputs).toTensor();

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  IValue res;
  for (int i = 0; i < 3; ++i) {
    res = bc.get_method("forward")(inputs);
  }
  auto output = res.toTensor();
  AT_ASSERT(outputref.dim() == output.dim());
  AT_ASSERT(
      outputref[0][0][0][0].item<int>() == output[0][0][0][0].item<int>());

  auto buff = save_mobile_module_to_bytes(bc);
  mobile::Module bc2 = parse_mobile_module(buff.data(), buff.size());
  for (int i = 0; i < 3; ++i) {
    res = bc2.get_method("forward")(inputs);
  }
  output = res.toTensor();
  AT_ASSERT(outputref.dim() == output.dim());
  AT_ASSERT(
      outputref[0][0][0][0].item<int>() == output[0][0][0][0].item<int>());
}

TEST(LiteInterpreterTest, Inline) {
  Module m("m");
  m.define(R"JIT(
  def foo1(self, x):
      return x + 1

  def foo2(self, x):
      return self.foo1(x) + 2

  def foo3(self, x):
      return self.foo2(x) + 3
  )JIT");
  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  std::vector<torch::jit::IValue> inputs({torch::ones({})});
  auto output = bc.get_method("foo3")(inputs);
  AT_ASSERT(output.toTensor().item<float>() == 7.0);

  auto buff = save_mobile_module_to_bytes(bc);
  mobile::Module bc2 = parse_mobile_module(buff.data(), buff.size());
  std::vector<torch::jit::IValue> inputs2({torch::ones({})});
  output = bc2.get_method("foo3")(inputs2);
  AT_ASSERT(output.toTensor().item<float>() == 7.0);
}

TEST(LiteInterpreterTest, Tuple) {
  Module m("m");
  m.define(R"JIT(
  def foo(self, x):
      return (1, 2, x + 3)

  def forward(self, x):
      tuple = self.foo(x)
      return tuple
  )JIT");
  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  std::vector<torch::jit::IValue> inputs({torch::ones({})});
  auto output = bc.get_method("forward")(inputs);
  AT_ASSERT(output.toTupleRef().elements()[1].toInt() == 2);

  auto buff = save_mobile_module_to_bytes(bc);
  mobile::Module bc2 = parse_mobile_module(buff.data(), buff.size());
  output = bc2.get_method("forward")(inputs);
  AT_ASSERT(output.toTuple()->elements()[1].toInt() == 2);
}

TEST(LiteInterpreterTest, Dict) {
  Module m("m");
  m.define(R"JIT(
  def foo(self, x):
      return {"result": x + 1}

  def forward(self, x):
      d = self.foo(x)
      return d
  )JIT");
  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  std::vector<torch::jit::IValue> inputs({torch::ones({})});
  auto output = bc.get_method("forward")(inputs);
  AT_ASSERT(output.toGenericDict().at("result").toTensor().item().toInt() == 2);

  auto buff = save_mobile_module_to_bytes(bc);
  mobile::Module bc2 = parse_mobile_module(buff.data(), buff.size());
  output = bc2.get_method("forward")(inputs);
  AT_ASSERT(output.toGenericDict().at("result").toTensor().item().toInt() == 2);
}

TEST(LiteInterpreterTest, PrimOverload) {
  /*
  // temporarily disabled
  script::Module m("m");
  m.define(R"JIT(
  def forward(self, x):
      result = [1, 2]
      result.append(3)
      return result
  )JIT");
  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  std::vector<torch::jit::IValue> inputs({torch::ones({})});
  auto output = bc.get_method("forward")(inputs);
  AT_ASSERT(output.toIntList()[2] == 3);
  */
}

TEST(LiteInterpreterTest, Prim) {
  Module m("m");
  m.define(R"JIT(
        def forward(self, x):
            return int(x)
  )JIT");

  std::vector<IValue> inputs;
  auto minput = 3.5 * torch::ones({});
  inputs.emplace_back(minput);
  auto ref = m.run_method("forward", minput);

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  IValue res;
  for (int i = 0; i < 3; ++i) {
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    auto bcinputs = inputs;
    res = bc.get_method("forward")(bcinputs);
  }

  auto resi = res.toInt();
  auto refi = ref.toInt();
  AT_ASSERT(resi == refi);

  auto buff = save_mobile_module_to_bytes(bc);
  mobile::Module bc2 = parse_mobile_module(buff.data(), buff.size());
  for (int i = 0; i < 3; ++i) {
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    auto bcinputs = inputs;
    res = bc2.get_method("forward")(bcinputs);
  }
  auto resi2 = res.toInt();
  AT_ASSERT(resi2 == refi);
}

TEST(LiteInterpreterTest, PrimScalar) {
  Module m("m");
  m.define(R"JIT(
        def forward(self, x):
            return int(x.item())
  )JIT");

  std::vector<IValue> inputs;
  auto minput = 3.5 * torch::ones({});
  inputs.emplace_back(minput);
  auto ref = m.run_method("forward", minput);

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  IValue res;
  for (int i = 0; i < 3; ++i) {
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    auto bcinputs = inputs;
    res = bc.get_method("forward")(bcinputs);
  }

  auto resi = res.toInt();
  auto refi = ref.toInt();
  AT_ASSERT(resi == refi);

  auto buff = save_mobile_module_to_bytes(bc);
  mobile::Module bc2 = parse_mobile_module(buff.data(), buff.size());
  for (int i = 0; i < 3; ++i) {
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    auto bcinputs = inputs;
    res = bc2.get_method("forward")(bcinputs);
  }
  auto resi2 = res.toInt();
  AT_ASSERT(resi2 == refi);
}

TEST(LiteInterpreterTest, LoadOrigJit) {
  Module m("m");
  m.register_parameter("foo", torch::ones({}), false);
  m.define(R"(
    def forward(self, x):
      b = 4
      return self.foo + x + b
  )");
  std::stringstream ss;
  m.save(ss);
  ASSERT_THROWS_WITH_MESSAGE(_load_for_mobile(ss), "file not found");
}

TEST(LiteInterpreterTest, WrongMethodName) {
  Module m("m");
  m.register_parameter("foo", torch::ones({}), false);
  m.define(R"(
    def add(self, x):
      b = 4
      return self.foo + x + b
  )");
  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  std::vector<IValue> inputs;
  auto minput = 5 * torch::ones({});
  inputs.emplace_back(minput);
  ASSERT_THROWS_WITH_MESSAGE(
      bc.get_method("forward")(inputs), "is not defined");

  auto buff = save_mobile_module_to_bytes(bc);
  mobile::Module bc2 = parse_mobile_module(buff.data(), buff.size());
  ASSERT_THROWS_WITH_MESSAGE(
      bc2.get_method("forward")(inputs), "is not defined");
}

TEST(LiteInterpreterTest, SetState) {
  Module m("m");
  m.register_parameter("foo", torch::ones({}), false);
  m.define(R"(
    def __getstate__(self):
      return self.foo
    def __setstate__(self, a):
      self.foo = a
    def forward(self, x):
      b = 4
      return self.foo + x + b
  )");

  std::vector<IValue> inputs;
  auto minput = 5 * torch::ones({});
  inputs.emplace_back(minput);

  std::stringstream ms;
  m.save(ms);
  auto loaded_m = load(ms);
  auto ref = loaded_m.run_method("forward", minput);

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  IValue res;
  for (int i = 0; i < 3; ++i) {
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    auto bcinputs = inputs;
    res = bc.get_method("forward")(bcinputs);
  }

  auto resd = res.toTensor().item<float>();
  auto refd = ref.toTensor().item<float>();
  AT_ASSERT(resd == refd);

  auto buff = save_mobile_module_to_bytes(bc);
  mobile::Module bc2 = parse_mobile_module(buff.data(), buff.size());
  for (int i = 0; i < 3; ++i) {
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    auto bcinputs = inputs;
    res = bc2.get_method("forward")(bcinputs);
  }

  auto resd2 = res.toTensor().item<float>();
  AT_ASSERT(resd2 == refd);
}

class TorchBindLiteInterpreterTestStruct
    : public torch::jit::CustomClassHolder {
 public:
  std::string get(at::Tensor t) {
    std::stringstream ss;
    ss << "Hello! Your tensor has ";
    ss << t.numel();
    ss << " elements!";
    return ss.str();
  }
};

namespace {
struct ClassNamespaceValue : public SugaredValue {
  explicit ClassNamespaceValue(c10::QualifiedName name)
      : basename_(std::move(name)) {}

  std::shared_ptr<SugaredValue> attr(
      const SourceRange& loc,
      GraphFunction& m,
      const std::string& name) override {
    const auto fullName = c10::QualifiedName(basename_, name);

    // Check to see if it is a custom class.
    if (auto custom_class = getCustomClass(fullName.qualifiedName())) {
      return std::make_shared<ClassValue>(custom_class);
    }

    // If it's not a custom class, assume it's another namespace
    // NOLINTNEXTLINE(performance-move-const-arg)
    return std::make_shared<ClassNamespaceValue>(std::move(fullName));
  }

  std::string kind() const override {
    return "Class Namespace";
  }

 private:
  c10::QualifiedName basename_;
};

struct TestModuleResolver : public Resolver {
  std::shared_ptr<SugaredValue> resolveValue(
      const std::string& name,
      GraphFunction& m,
      const SourceRange& loc) override {
    if (name == "torch") {
      return std::make_shared<BuiltinModule>("aten");
    } else if (name == "__torch__") {
      return std::make_shared<ClassNamespaceValue>(c10::QualifiedName(name));
    }

    return nullptr;
  }

  TypePtr resolveType(const std::string& name, const SourceRange& loc)
      override {
    return nullptr;
  }
};
} // namespace

TEST(LiteInterpreterTest, BuiltinClass) {
  script::Module m("m");

  auto cls = getCustomClass(
      "__torch__.torch.classes._TorchScriptTesting._LiteInterpreterTest");
  TORCH_INTERNAL_ASSERT(cls);
  c10::intrusive_ptr<torch::CustomClassHolder> obj_holder;
  m.register_attribute("my_obj", cls, IValue::make_capsule(obj_holder));

  m.register_parameter("foo", torch::ones({}), false);
  m.define(
      R"(
    def __getstate__(self):
      return 1
    def __setstate__(self, a):
      self.my_obj = __torch__.torch.classes._TorchScriptTesting._LiteInterpreterTest()

    def forward(self, x) -> str:
      return self.my_obj.get(x)
  )",
      std::make_shared<TestModuleResolver>());

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  auto res =
      bc.get_method("forward")(std::vector<IValue>{torch::zeros({3, 4})});
  const auto& str = res.toStringRef();
  std::string expected = "Hello! Your tensor has 12 elements!";
  AT_ASSERT(str == expected);

  auto buff = save_mobile_module_to_bytes(bc);
  mobile::Module bc2 = parse_mobile_module(buff.data(), buff.size());
  res = bc2.get_method("forward")(std::vector<IValue>{torch::zeros({3, 4})});
  const auto& str2 = res.toStringRef();
  AT_ASSERT(str2 == expected);
}

TEST(LiteInterpreterTest, BuiltinFunction) {
  script::Module m("m");
  auto custom_class_obj =
      make_custom_class<TorchBindLiteInterpreterTestStruct>();
  m.register_attribute("my_obj", custom_class_obj.type(), custom_class_obj);
  m.define(R"(
    def forward(self, x) -> str:
      return self.my_obj.get(x)
  )");

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  auto res =
      bc.get_method("forward")(std::vector<IValue>{torch::zeros({3, 4})});
  // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
  auto str = res.toStringRef();
  std::string expected = "Hello! Your tensor has 12 elements!";
  AT_ASSERT(str == expected);

  auto buff = save_mobile_module_to_bytes(bc);
  mobile::Module bc2 = parse_mobile_module(buff.data(), buff.size());
  res = bc2.get_method("forward")(std::vector<IValue>{torch::zeros({3, 4})});
  // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
  str = res.toStringRef();
  AT_ASSERT(str == expected);
}

#if !defined FB_XPLAT_BUILD
TEST(LiteInterpreterTest, GetRuntimeByteCodeVersion) {
  auto runtime_bytecode_version = _get_runtime_bytecode_version();
  AT_ASSERT(
      runtime_bytecode_version ==
      caffe2::serialize::kMaxSupportedBytecodeVersion);
}

TEST(LiteInterpreterTest, GetRuntimeOperatorsVersion) {
  auto runtime_operators_version = _get_runtime_operators_min_max_versions();
  AT_ASSERT(
      runtime_operators_version.first ==
          caffe2::serialize::kMinSupportedFileFormatVersion &&
      runtime_operators_version.second ==
          caffe2::serialize::kMaxSupportedFileFormatVersion);
}

/**
 * The test below is disarmed for FB internal xplat builds since
 * BUCK requires us to pass in the script_module_v4.ptl file in
 * as a resource dependency of the build rule for this file, and
 * we would need to access it via the C++ Resources API instead
 * of directly reading from disk (which is what the open source
 * build/run does).
 */
TEST(LiteInterpreterTest, GetByteCodeVersion) {
  std::string filePath(__FILE__);
  auto test_model_file_v4 =
      filePath.substr(0, filePath.find_last_of("/\\") + 1);
  test_model_file_v4.append("script_module_v4.ptl");

  auto version_v4 = _get_model_bytecode_version(test_model_file_v4);
  AT_ASSERT(version_v4 == 4);
}

#endif // !defined(FB_XPLAT_BUILD)

TEST(LiteInterpreterTest, GetContainTypes) {
  Module m("m");
  m.define(R"(
    def forward(self):
      return 3
  )");

  std::stringstream ss;
  m._save_for_mobile(ss, {}, true);

  auto contained_types = _get_mobile_model_contained_types(ss);
  AT_ASSERT(contained_types.size() >= 0);
}

namespace {

void compareModelOutput(
    c10::ArrayRef<IValue> actual_result_list,
    const std::vector<Tensor>& expect_result_list) {
  AT_ASSERT(actual_result_list.size() == expect_result_list.size());
  AT_ASSERT(actual_result_list[0].toTensor().equal(expect_result_list[0]));
  AT_ASSERT(
      actual_result_list[1].toTensor().dim() == expect_result_list[1].dim());
  AT_ASSERT(actual_result_list[2].toTensor().equal(expect_result_list[2]));
  AT_ASSERT(actual_result_list[3].toTensor().equal(expect_result_list[3]));
}

void runAndCheckTorchScriptModel(
    std::stringstream& input_model_stream,
    const std::vector<IValue>& input_data,
    const std::vector<Tensor>& expect_result_list,
    const int64_t expect_version) {
  auto actual_version = _get_model_bytecode_version(input_model_stream);
  AT_ASSERT(actual_version == expect_version);

  // Load and run the backport model, then compare the result with expect
  // result
  Module m_mobile = load(input_model_stream);

  auto actual_result = m_mobile.forward(input_data);
  const auto& actual_result_list = actual_result.toTupleRef().elements();
  compareModelOutput(actual_result_list, expect_result_list);
}

void runAndCheckBytecodeModel(
    std::stringstream& input_model_stream,
    const std::vector<IValue>& input_data,
    const std::vector<Tensor>& expect_result_list,
    const int64_t expect_version) {
  auto actual_version = _get_model_bytecode_version(input_model_stream);
  AT_ASSERT(actual_version == expect_version);

  // Load and run the backport model, then compare the result with expect
  // result
  Module m_mobile = load(input_model_stream);

  auto actual_result = m_mobile.forward(input_data);
  const auto& actual_result_list = actual_result.toTupleRef().elements();

  compareModelOutput(actual_result_list, expect_result_list);
}

void backportAllVersionCheck(
    std::stringstream& test_model_file_stream,
    std::vector<IValue>& input_data,
    std::vector<Tensor>& expect_result_list,
    const int64_t expect_from_version) {
  auto from_version = _get_model_bytecode_version(test_model_file_stream);
  AT_ASSERT(from_version == expect_from_version);

  // Backport script_module_v5.ptl to an older version
  constexpr int64_t minimum_to_version = 4;
  int64_t current_to_version = from_version - 1;

  // Verify all candidate to_version work as expected. All backport to version
  // larger than minimum_to_version should success.
  while (current_to_version >= minimum_to_version) {
    // Do not declare std::stringstream oss outside of the while loop as
    // oss.clear() doesn't reset the stream content, only clears out error state
    // flag in stringstream causing a problematic stream. Instead, it's cleaner
    // and safer to just declare a new std::stringstream one and swap them.
    std::stringstream oss;
    bool backPortSuccess =
        _backport_for_mobile(test_model_file_stream, oss, current_to_version);
    AT_ASSERT(backPortSuccess);

    // Check backport model version
    auto backport_version = _get_model_bytecode_version(oss);
    AT_ASSERT(backport_version == current_to_version);

    // Load and run the backport model, then compare the result with expect
    // result
    runAndCheckBytecodeModel(
        oss, input_data, expect_result_list, current_to_version);
    runAndCheckTorchScriptModel(
        oss, input_data, expect_result_list, current_to_version);

    current_to_version--;
  }
  //  backport to minimum version - 1 should fail
  std::stringstream oss;
  bool backPortSuccess =
      _backport_for_mobile(test_model_file_stream, oss, minimum_to_version - 1);
  AT_ASSERT(!backPortSuccess);
}
} // namespace

#if !defined FB_XPLAT_BUILD
TEST(LiteInterpreterTest, BackPortByteCodeModelAllVersions) {
  torch::jit::Module module("m");
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
  module.register_parameter("weight", torch::ones({20, 1, 5, 5}), false);
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
  module.register_parameter("bias", torch::ones({20}), false);
  module.define(R"(
    def forward(self, input):
      x1 = torch.zeros(2, 2)
      x2 = torch.empty_like(torch.empty(2, 2))
      x3 = torch._convolution(input, self.weight, self.bias, [1, 1], [0, 0], [1, 1], False, [0, 0], 1, False, False, True, True)
      # Add torch.add operator to cover bytecode version bump from 6 to 7
      # for bytecode version 7, the main change is to support defaults arguments with out arguments
      x = 2 * torch.ones(1)
      h = torch.ones(1)
      torch.add(x, h, out=x)
      return (x1, x2, x3, x)
  )");

  torch::jit::Module module_freeze = freeze(module);

  std::stringstream input_model_stream;
  module_freeze._save_for_mobile(input_model_stream);
  std::vector<IValue> input_data =
      std::vector<IValue>({torch::ones({1, 1, 28, 28})});
  std::vector<Tensor> expect_result_list;
  expect_result_list.emplace_back(at::ones({2, 2}, ScalarType::Float) * 0);
  expect_result_list.emplace_back(at::ones({2, 2}, ScalarType::Float));
  expect_result_list.emplace_back(
      at::ones({1, 20, 24, 24}, ScalarType::Float) * 26);
  expect_result_list.emplace_back(3 * at::ones({1}));

  backportAllVersionCheck(
      input_model_stream,
      input_data,
      expect_result_list,
      caffe2::serialize::kProducedBytecodeVersion);
}
#endif // !defined(FB_XPLAT_BUILD)

TEST(LiteInterpreterTest, GetRuntimeOpsAndInfo) {
  auto runtime_ops = _get_runtime_ops_and_info();
  // Ballpark estimate of the minimal number of ops; just used to
  // verify API returns a reasonably large number.
  AT_ASSERT(runtime_ops.size() > 2900);
}

TEST(LiteInterpreterTest, isCompatibleSuccess) {
  // test trivial success case
  auto runtime_info = RuntimeCompatibilityInfo::get();
  std::unordered_map<std::string, OperatorInfo> model_ops;
  model_ops["aten::add.Scalar"] = OperatorInfo{2};

  std::unordered_set<std::string> types = {"List", "int", "NamedTuple"};
  auto model_info = ModelCompatibilityInfo{
      caffe2::serialize::kMaxSupportedBytecodeVersion,
      model_ops,
      types,
      _get_runtime_bytecode_min_max_versions().first};

  AT_ASSERT(
      is_compatible(runtime_info, model_info).status ==
      ModelCompatibilityStatus::OK);
}

TEST(LiteInterpreterTest, isCompatibleFail) {
  // test trivial failure due to ops
  std::unordered_map<std::string, OperatorInfo> model_ops;
  model_ops["aten::add.Scalar"] = OperatorInfo{2};
  auto model_info = ModelCompatibilityInfo{
      caffe2::serialize::kMaxSupportedBytecodeVersion, model_ops};
  std::unordered_map<std::string, OperatorInfo> runtime_ops;
  runtime_ops["aten::add.Int"] = OperatorInfo{2};
  auto runtime_info = RuntimeCompatibilityInfo{
      std::pair<uint64_t, uint64_t>(
          caffe2::serialize::kMinSupportedBytecodeVersion,
          caffe2::serialize::kMaxSupportedBytecodeVersion),
      runtime_ops,
      _get_mobile_supported_types()};

  auto result = is_compatible(runtime_info, model_info);
  AT_ASSERT(result.status = ModelCompatibilityStatus::ERROR);
  AT_ASSERT(
      result.errors[0] ==
      "Operator 'aten::add.Scalar' missing from runtime (not found)");

  // test trivial failure due to bytecode greater than max supported bytecode
  // version
  runtime_ops["aten::add.Scalar"] = OperatorInfo{2};
  runtime_info = RuntimeCompatibilityInfo{
      std::pair<uint64_t, uint64_t>(
          caffe2::serialize::kMinSupportedBytecodeVersion,
          caffe2::serialize::kMaxSupportedBytecodeVersion),
      runtime_ops,
      _get_mobile_supported_types()};
  model_info.bytecode_version =
      caffe2::serialize::kMaxSupportedBytecodeVersion + 1;

  result = is_compatible(runtime_info, model_info);
  AT_ASSERT(result.status = ModelCompatibilityStatus::ERROR);

  // test trivial failure due to bytecode less than min supported bytecode
  // version
  runtime_ops["aten::add.Scalar"] = OperatorInfo{2};
  runtime_info = RuntimeCompatibilityInfo{
      std::pair<uint64_t, uint64_t>(
          caffe2::serialize::kMinSupportedBytecodeVersion,
          caffe2::serialize::kMaxSupportedBytecodeVersion),
      runtime_ops,
      _get_mobile_supported_types()};
  model_info.bytecode_version =
      caffe2::serialize::kMinSupportedBytecodeVersion - 1;

  result = is_compatible(runtime_info, model_info);
  AT_ASSERT(result.status = ModelCompatibilityStatus::ERROR);

  // test trivial failure due to type
  runtime_info = RuntimeCompatibilityInfo::get();
  std::unordered_set<std::string> types = {"List", "int", "Sequence"};

  model_info = ModelCompatibilityInfo{
      caffe2::serialize::kMaxSupportedBytecodeVersion,
      model_ops,
      types,
      _get_runtime_bytecode_min_max_versions().first};

  AT_ASSERT(
      is_compatible(runtime_info, model_info).status ==
      ModelCompatibilityStatus::ERROR);

  // test trivial failure due to operator version
  runtime_info = RuntimeCompatibilityInfo::get();

  model_info = ModelCompatibilityInfo{
      caffe2::serialize::kMaxSupportedBytecodeVersion, model_ops, {}, 0};

  AT_ASSERT(
      is_compatible(runtime_info, model_info).status ==
      ModelCompatibilityStatus::ERROR);
}

TEST(LiteInterpreterTest, Eval) {
  std::vector<torch::jit::IValue> inputs;

  Module m("m");
  m.define(R"(
    def __init__(self, x):
      self.training = True

    def forward(self, input):
      return torch.dropout(input, 1.0, self.training)
  )");

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers,modernize-use-emplace)
  inputs.push_back(torch::ones({1, 1, 28, 28}));
  m.eval();
  auto outputref = m.forward(inputs).toTensor();

  // save m in training mode to make sure that mobile eval() will correctly
  // change back to eval mode
  m.train();
  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  bc.eval();
  IValue res;
  for (int i = 0; i < 3; ++i) {
    res = bc.get_method("forward")(inputs);
  }
  auto output = res.toTensor();
  AT_ASSERT(outputref.dim() == output.dim());
  AT_ASSERT(
      outputref[0][0][0][0].item<int>() == output[0][0][0][0].item<int>());

  auto buff = save_mobile_module_to_bytes(bc);
  mobile::Module bc2 = parse_mobile_module(buff.data(), buff.size());
  bc2.eval();
  for (int i = 0; i < 3; ++i) {
    res = bc2.get_method("forward")(inputs);
  }
  output = res.toTensor();
  AT_ASSERT(outputref.dim() == output.dim());
  AT_ASSERT(
      outputref[0][0][0][0].item<int>() == output[0][0][0][0].item<int>());
}

TEST(LiteInterpreterTest, FindWrongMethodName) {
  Module m("m");
  m.register_parameter("foo", torch::ones({}), false);
  m.define(R"(
    def add(self, x):
      b = 4
      return self.foo + x + b
  )");
  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  ASSERT_TRUE(bc.find_method("forward") == c10::nullopt);

  auto buff = save_mobile_module_to_bytes(bc);
  mobile::Module bc2 = parse_mobile_module(buff.data(), buff.size());
  ASSERT_TRUE(bc2.find_method("forward") == c10::nullopt);
}

TEST(LiteInterpreterTest, FindAndRunMethod) {
  Module m("m");
  m.register_parameter("foo", torch::ones({}), false);
  m.define(R"(
    def add_it(self, x):
      b = 4
      return self.foo + x + b
  )");

  std::vector<IValue> inputs;
  auto minput = 5 * torch::ones({});
  inputs.emplace_back(minput);
  auto ref = m.get_method("add_it")(inputs);

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  IValue res;
  for (int i = 0; i < 3; ++i) {
    auto bcinputs = inputs;
    auto method = bc.find_method("add_it");
    AT_ASSERT(method != c10::nullopt);
    res = (*method)(std::move(bcinputs));
  }

  auto resd = res.toTensor().item<float>();
  auto refd = ref.toTensor().item<float>();
  AT_ASSERT(resd == refd);

  auto buff = save_mobile_module_to_bytes(bc);
  mobile::Module bc2 = parse_mobile_module(buff.data(), buff.size());

  for (int i = 0; i < 3; ++i) {
    auto bcinputs = inputs;
    auto method = bc2.find_method("add_it");
    AT_ASSERT(method != c10::nullopt);
    res = (*method)(std::move(bcinputs));
  }

  resd = res.toTensor().item<float>();
  AT_ASSERT(resd == refd);
}

TEST(LiteInterpreterTest, RunMethodVariadic) {
  Module m("m");
  m.register_parameter("foo", torch::ones({}), false);
  m.define(R"(
    def add_three(self, x, y):
      return self.foo + x + y
  )");

  std::vector<IValue> inputs;
  auto inputx = 5 * torch::ones({});
  auto inputy = 4 * torch::ones({});
  auto ref = m.run_method("add_three", inputx, inputy);

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  IValue res = bc.run_method("add_three", inputx, inputy);

  auto resd = res.toTensor().item<float>();
  auto refd = ref.toTensor().item<float>();
  AT_ASSERT(resd == refd);

  auto buff = save_mobile_module_to_bytes(bc);
  mobile::Module bc2 = parse_mobile_module(buff.data(), buff.size());
  res = bc.run_method("add_three", inputx, inputy);
  resd = res.toTensor().item<float>();
  AT_ASSERT(resd == refd);
}

TEST(LiteInterpreterTest, DuplicateSetState) {
  Module m("M");
  m.register_parameter("foo", torch::ones({}), false);
  m.define(R"(
    def __getstate__(self):
      return self.foo + self.foo
    def __setstate__(self, a):
      self.foo = a
    def forward(self, x):
      b = 4
      return self.foo + x + b
  )");

  Module b("B");
  b.register_module("M0", m);
  b.register_module("M1", m);
  b.define(R"(
    def forward(self, x):
      return self.M0.forward(x) + self.M1.forward(x)
  )");

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  const auto methods = bc.get_methods();
  const size_t expected_n = 3;
  ASSERT_EQ(methods.size(), expected_n);

  auto buff = save_mobile_module_to_bytes(bc);
  mobile::Module bc2 = parse_mobile_module(buff.data(), buff.size());
  const auto methods2 = bc.get_methods();
  ASSERT_EQ(methods2.size(), expected_n);
}

TEST(LiteInterpreterTest, ExtraFiles) {
  const auto script = R"JIT(
    def forward(self):
        x = torch.rand(5, 5)
        x = x.mm(x)
        return x
  )JIT";

  auto module =
      std::make_shared<Module>("Module", std::make_shared<CompilationUnit>());
  module->define(script);
  std::ostringstream oss;
  std::unordered_map<std::string, std::string> extra_files;
  extra_files["metadata.json"] = "abc";
  extra_files["mobile_info.json"] = "{\"key\": 23}";
  module->_save_for_mobile(oss, extra_files);

  std::istringstream iss(oss.str());
  caffe2::serialize::IStreamAdapter adapter{&iss};
  std::unordered_map<std::string, std::string> loaded_extra_files;
  loaded_extra_files["metadata.json"] = "";
  torch::jit::_load_for_mobile(iss, torch::kCPU, loaded_extra_files);
  ASSERT_EQ(loaded_extra_files["metadata.json"], "abc");

  loaded_extra_files.clear();
  std::vector<std::string> all_files =
      caffe2::serialize::PyTorchStreamReader(&iss).getAllRecords();

  for (auto& file_name : all_files) {
    if (file_name.find("extra/") == 0) {
      loaded_extra_files[file_name.substr(6)] = "";
    }
  }

  torch::jit::_load_for_mobile(iss, torch::kCPU, loaded_extra_files);
  ASSERT_EQ(loaded_extra_files["metadata.json"], "abc");
  ASSERT_EQ(loaded_extra_files["mobile_info.json"], "{\"key\": 23}");
}

TEST(LiteInterpreterTest, OpNameExportFetchRootOperators) {
  torch::jit::Module m("m");
  m.register_parameter("weight", torch::ones({20, 1, 5, 5}), false);
  m.register_parameter("bias", torch::ones({20}), false);
  m.define(R"(
    def forward(self, input):
      x1 = torch.zeros(2, 2)
      x2 = torch.empty_like(torch.empty(2, 2))
      x3 = torch._convolution(input, self.weight, self.bias, [1, 1], [0, 0], [1, 1], False, [0, 0], 1, False, False, True, True)
      return (x1, x2, x3)
  )");
  m.eval();

  std::stringstream ss;
  m._save_for_mobile(ss);

  torch::jit::mobile::Module ptl_model = torch::jit::_load_for_mobile(ss);
  std::set<std::string> operator_names =
      torch::jit::mobile::_export_operator_list(ptl_model);
  std::set<std::string> expected_operator_names = {
      "aten::_convolution",
      "aten::empty.memory_format",
      "aten::empty_like",
      "aten::zeros",
  };
  EXPECT_EQ(operator_names, expected_operator_names)
      << "Expected the root operator lists to be the same";

  auto buff = save_mobile_module_to_bytes(ptl_model);
  mobile::Module bc2 = parse_mobile_module(buff.data(), buff.size());
  operator_names = torch::jit::mobile::_export_operator_list(bc2);
  EXPECT_EQ(operator_names, expected_operator_names)
      << "Expected the root operator lists to be the same";
}

TEST(LiteInterpreterTest, DefaultArgsConv) {
  auto s = std::getenv("PYTORCH_TEST_WITH_TSAN");
  if (s && strcmp(s, "1") == 0)
    return;

  std::vector<torch::jit::IValue> inputs;

  Module m("m");
  m.register_parameter("weight", torch::ones({20, 1, 5, 5}), false);
  m.register_parameter("bias", torch::ones({20}), false);
  m.define(R"(
    def forward(self, input):
      return torch.conv2d(input, self.weight, self.bias, [1, 1], [0, 0], [1, 1], 1)
  )");

  inputs.emplace_back(torch::ones({1, 1, 28, 28}));

  auto outputref = m.forward(inputs).toTensor();

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  IValue res;
  for (int i = 0; i < 1; ++i) {
    res = bc.get_method("forward")(inputs);
  }
  auto output = res.toTensor();
  AT_ASSERT(outputref.dim() == output.dim());
  AT_ASSERT(output.equal(outputref));

  auto buff = save_mobile_module_to_bytes(bc);
  mobile::Module bc2 = parse_mobile_module(buff.data(), buff.size());
  for (int i = 0; i < 1; ++i) {
    res = bc2.get_method("forward")(inputs);
  }
  output = res.toTensor();
  AT_ASSERT(outputref.dim() == output.dim());
  AT_ASSERT(output.equal(outputref));
}

TEST(RunTimeTest, ParseBytecode) {
  // A simple example to show a simple bytecode that can be used independent of
  // PyTorch TorchScript serialization (unpickler, etc) and operator library.
  // It has basic control flow (if, else) and basic data orchestration (list
  // construction). The original PyTorch program:

  //  class Module(torch.nn.Module):
  //
  //    def __init__(self):
  //      super().__init__()
  //
  //    def forward(self, x: int, h: int, xfirst: bool):
  //      if xfirst:
  //        return [x, h]
  //      else:
  //        return [h, x]

  // 1. Prepare for the bytecode. In reality it can be from a customized
  // deserializer.
  std::vector<IValue> instructions{
      to_tuple({"STOREN", 1, 4}),
      to_tuple({"DROPR", 1, 0}),
      to_tuple({"MOVE", 4, 0}),
      to_tuple({"JF", 5, 0}),
      to_tuple({"LOAD", 2, 0}),
      to_tuple({"LOAD", 3, 0}),
      to_tuple({"LIST_CONSTRUCT", 0, 2}),
      to_tuple({"JMP", 4, 0}),
      to_tuple({"LOAD", 3, 0}),
      to_tuple({"LOAD", 2, 0}),
      to_tuple({"LIST_CONSTRUCT", 1, 2}),
      to_tuple({"STORE", 5, 0}),
      to_tuple({"DROPR", 3, 0}),
      to_tuple({"DROPR", 2, 0}),
      to_tuple({"MOVE", 5, 0}),
      to_tuple({"RET", 0, 0}),
  };
  std::vector<IValue> operators; // empty for this example
  std::vector<IValue> constants; // empty for this example

  std::vector<IValue> types{"List[int]", "List[int]"};
  // 2. Parse the function
  std::string function_name("test_function");
  auto function =
      std::make_unique<mobile::Function>(c10::QualifiedName(function_name));
  c10::ivalue::TupleElements debug_handles_m_tuple;
  parseInstructions(
      function_name,
      std::move(*c10::ivalue::Tuple::create(instructions)).elements(),
      debug_handles_m_tuple,
      function.get());
  parseTypes(c10::ivalue::Tuple::create(types)->elements(), function.get());
  const size_t rsize = 5;
  parseRegisterSize(rsize, function.get());

  // 3. Prepare for inputs and run the function
  // Note that the first input is reserved for Module object.
  // Since this is a function test and Module object is not required,
  // a dummy IValue (0) is added here.
  std::vector<IValue> inputs{0, 1, 2, true};
  function->run(inputs);
  auto output = inputs[0].toList();
  ASSERT_EQ(output[0], 1);
  ASSERT_EQ(output[1], 2);

  std::vector<IValue> inputs1{0, 1, 2, false};
  function->run(inputs1);
  auto output1 = inputs1[0].toList();
  ASSERT_EQ(output1[0], 2);
  ASSERT_EQ(output1[1], 1);
}

TEST(RunTimeTest, ParseOperator) {
  // A simple example to show a simple bytecode that can be used independent of
  // PyTorch TorchScript serialization (unpickler, etc) and operator library.
  // It has one operator and we should be able to register it. The original
  // PyTorch program:

  // class Add(torch.nn.Module):
  //     def __init__(self):
  //         super(Add, self).__init__()

  //     def forward(self, a, b):
  //         return a + b

  // 1. Prepare for the bytecode. In reality it can be from a customized
  // deserializer.
  std::vector<IValue> instructions{
      to_tuple({"STOREN", 1, 3}),
      to_tuple({"DROPR", 1, 0}),
      to_tuple({"MOVE", 2, 0}),
      to_tuple({"MOVE", 3, 0}),
      to_tuple({"OP", 0, 0}),
      to_tuple({"RET", 0, 0}),
  };
  std::vector<IValue> operators{
      to_tuple({"aten::add", "Tensor", 2}),
  };
  std::vector<IValue> constants{
      to_tuple({1}),
  };
  int64_t model_version = caffe2::serialize::kProducedBytecodeVersion;
  // 2. Parse the function
  std::string function_name("test_function");
  auto function =
      std::make_unique<mobile::Function>(c10::QualifiedName(function_name));
  c10::ivalue::TupleElements debug_handles_m_tuple;
  parseInstructions(
      function_name,
      std::move(*c10::ivalue::Tuple::create(instructions)).elements(),
      debug_handles_m_tuple,
      function.get());
  parseOperators(
      std::move(*c10::ivalue::Tuple::create(operators)).elements(),
      model_version,
      1,
      function.get());
  const size_t rsize = 5;
  parseRegisterSize(rsize, function.get());

  // 3. Prepare for inputs and run the function
  // Note that the first input is reserved for Module object.
  // Since this is a function test and Module object is not required,
  // a dummy IValue (0) is added here.
  std::vector<IValue> inputs{0, at::tensor(1), at::tensor(2)};
  function->run(inputs);
  auto output = inputs[0];
  ASSERT_EQ(output, at::tensor(3));
}

namespace {
void testLiteModuleCompareResultTensors(
    Module& m,
    const std::vector<torch::jit::IValue>& inputs,
    const std::string& method_name = "forward") {
  auto outputref = m.get_method(method_name)(inputs).toTensor();

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  IValue res;
  for (int i = 0; i < 3; ++i) {
    res = bc.get_method(method_name)(inputs);
  }
  auto output = res.toTensor();
  AT_ASSERT(outputref.dim() == output.dim());
  AT_ASSERT(output.equal(outputref));

  auto buff = save_mobile_module_to_bytes(bc);
  mobile::Module bc2 = parse_mobile_module(buff.data(), buff.size());
  for (int i = 0; i < 3; ++i) {
    res = bc2.get_method(method_name)(inputs);
  }
  output = res.toTensor();
  AT_ASSERT(outputref.dim() == output.dim());
  AT_ASSERT(output.equal(outputref));
}

void testDefaultArgsPinv(int num_args) {
  Module m("m");
  if (num_args == 1) {
    m.define(R"(
      def forward(self, input):
        return torch.linalg_pinv(input)
    )");
  } else if (num_args == 2) {
    m.define(R"(
      def forward(self, input):
        return torch.linalg_pinv(input, 1e-5)
    )");
  } else if (num_args == 3) {
    m.define(R"(
      def forward(self, input):
        return torch.linalg_pinv(input, 1e-5, True)
    )");
  }

  std::vector<torch::jit::IValue> inputs;
  const int N = 28;
  auto input = torch::range(1, N * N, 1);
  input[0] = 1; // a more stable matrix
  input = input.view({N, N});
  inputs.emplace_back(input);
  testLiteModuleCompareResultTensors(m, inputs);
}
} // namespace

#if !defined FB_XPLAT_BUILD
TEST(LiteInterpreterTest, DefaultArgsPinv) {
  // Test with different number of specified arguments.
  // Arguments not specified take default value.
  for (int num_args = 1; num_args <= 3; ++num_args) {
    testDefaultArgsPinv(num_args);
  }

  //  bytecode with one specified argument:
  //  (6,
  //      ('__torch__.m.forward',
  //          (('instructions',
  //              (('STOREN', 1, 2),
  //                  ('DROPR', 1, 0),
  //                  ('MOVE', 2, 0),
  //                  ('OP', 0, 0),
  //                  ('RET', 0, 0))),
  //              ('operators', (('aten::linalg_pinv', '', 1),)),
  //              ('constants', (False, 1e-15)), # default constants are not
  //              used
  //              ('types', ()),
  //              ('register_size', 2)),
  //          (('arguments',
  //              ((('name', 'self'), ('type', '__torch__.m'), ('default_value',
  //              None)),
  //                  (('name', 'input'), ('type', 'Tensor'), ('default_value',
  //                  None)))),
  //              ('returns',
  //                  ((('name', ''), ('type', 'Tensor'), ('default_value',
  //                  None)),)))))

  //  bytecode with 2 specified argument:
  //  (6,
  //      ('__torch__.m.forward',
  //          (('instructions',
  //              (('STOREN', 1, 2),
  //                  ('DROPR', 1, 0),
  //                  ('MOVE', 2, 0),
  //                  ('LOADC', 1, 0), # added LOADC for specified argument
  //                  ('OP', 0, 0),
  //                  ('RET', 0, 0))),
  //              ('operators', (('aten::linalg_pinv', '', 2),)),
  //              ('constants', (False, 1e-05)), # updated constant table
  //              ('types', ()),
  //              ('register_size', 2)),
  //          (('arguments',
  //              ((('name', 'self'), ('type', '__torch__.m'), ('default_value',
  //              None)),
  //                  (('name', 'input'), ('type', 'Tensor'), ('default_value',
  //                  None)))),
  //              ('returns',
  //                  ((('name', ''), ('type', 'Tensor'), ('default_value',
  //                  None)),)))))

  //  bytecode with 3 specified arguments:
  //  (6,
  //      ('__torch__.m.forward',
  //          (('instructions',
  //              (('STOREN', 1, 2),
  //                  ('DROPR', 1, 0),
  //                  ('MOVE', 2, 0),
  //                  ('LOADC', 1, 0),
  //                  ('LOADC', 0, 0),
  //                  ('OP', 0, 0),
  //                  ('RET', 0, 0))),
  //              ('operators', (('aten::linalg_pinv', '', 3),)),
  //              ('constants', (True, 1e-05)),
  //              ('types', ()),
  //              ('register_size', 2)),
  //          (('arguments',
  //              ((('name', 'self'), ('type', '__torch__.m'), ('default_value',
  //              None)),
  //                  (('name', 'input'), ('type', 'Tensor'), ('default_value',
  //                  None)))),
  //              ('returns',
  //                  ((('name', ''), ('type', 'Tensor'), ('default_value',
  //                  None)),)))))
}

TEST(LiteInterpreterTest, DefaultArgsTensorinvSpecifyDefault) {
  // The second argument is specified, but the value is the same as the default
  // value. It's treated as "not specified" since the value can be fetched from
  // schema.
  Module m("m");
  m.define(R"(
    def forward(self, input):
      return torch.linalg_tensorinv(input, 2)
  )");
  torch::jit::MobileCode code(m.get_method("forward").graph(), "forward");
  auto arg_nums = code.op_to_num_specified_args();
  ASSERT_EQ(arg_nums.size(), 1);
  ASSERT_EQ(arg_nums["aten::linalg_tensorinv"], 1);
  std::vector<torch::jit::IValue> inputs;
  const int N = 4;
  auto input = torch::rand({N, N, N, N});
  inputs.emplace_back(input);
  testLiteModuleCompareResultTensors(m, inputs);
}

void testDefaultArgsPinvWithOutArg(int num_args) {
  Module m("m");
  if (num_args == 1) {
    m.define(R"(
      def forward(self, input):
        return torch.linalg_pinv(input, out=input)
    )");
  } else if (num_args == 2) {
    m.define(R"(
      def forward(self, input):
        return torch.linalg_pinv(input, 1e-5, out=input)
    )");
  } else if (num_args == 3) {
    m.define(R"(
      def forward(self, input):
        return torch.linalg_pinv(input, 1e-5, True, out=input)
    )");
  }

  const int N = 28;
  auto input = torch::range(1, N * N, 1);
  input[0] = 10000; // a more stable matrix
  input = input.view({N, N});
  auto ref = m.run_method("forward", input);
  TORCH_CHECK(!input.equal(torch::range(1, N * N, 1)));
  TORCH_CHECK(input.equal(ref.toTensor()));
}

TEST(LiteInterpreterTest, DefaultArgsPinvWithOutArg) {
  // Test with different number of specified arguments + out arg.
  // Arguments not specified take default value.
  for (int num_args = 1; num_args <= 3; ++num_args) {
    testDefaultArgsPinvWithOutArg(num_args);
  }
}

TEST(LiteInterpreterTest, DefaultArgsWithOutArg) {
  Module m("m");
  m.define(R"(
    def forward(self, x, h):
      torch.add(x, h, out=x)
  )");

  std::vector<IValue> inputs;
  auto input_x = 2 * torch::ones({});
  auto input_h = torch::ones({});
  auto ref = m.run_method("forward", input_x, input_h);

  std::stringstream ss;

  m._save_for_mobile(ss, {}, true);
  mobile::Module bc = _load_for_mobile(ss);
  bc.run_method("forward", input_x, input_h);
  AT_ASSERT(input_x.equal(4 * torch::ones({})));

  auto ops = _get_model_ops_and_info(ss);
  auto op = ops.find("aten::add.out");
  TORCH_CHECK(
      op != ops.end() && op->second.num_schema_args.has_value() &&
      op->second.num_schema_args.value() == 3);

  auto buff = save_mobile_module_to_bytes(bc);
  mobile::Module bc2 = parse_mobile_module(buff.data(), buff.size());
  auto input_x2 = 2 * torch::ones({});
  auto input_h2 = torch::ones({});
  m.run_method("forward", input_x2, input_h2);
  bc2.run_method("forward", input_x2, input_h2);
  AT_ASSERT(input_x2.equal(4 * torch::ones({})));
  ops = _get_model_ops_and_info(ss);
  op = ops.find("aten::add.out");
  TORCH_CHECK(
      op != ops.end() && op->second.num_schema_args.has_value() &&
      op->second.num_schema_args.value() == 3);
}

TEST(LiteInterpreterTest, TestExceptionStackWithTwoLevelModuleHierarchy) {
  Module a("A");
  a.define(R"(
    def bar(self, x, y):
      return x + y
  )");
  Module b("B");
  b.register_module("A0", a);
  b.define(R"(
    def foo(self, x, y):
      return self.A0.bar(x, y) + 2
  )");
  Module c("C");
  c.register_module("B0", b);
  c.define(R"(
    def forward(self, x, y):
      return self.B0.foo(x, y) + 3
  )");

  std::vector<IValue> inputs;
  inputs.emplace_back(torch::rand({2, 4}));
  inputs.emplace_back(torch::rand({13, 9}));

  std::stringstream ss;
  c._save_for_mobile(ss, ExtraFilesMap(), true);
  auto lite_m = _load_for_mobile(ss);
  std::string error_pattern = R"(
  Module hierarchy:top(C)::<unknown>.B0(B)::foo.A0(A)::bar.aten::add
Traceback of TorchScript (most recent call last):
  File "<string>", line 3, in <unknown>

    def forward(self, x, y):
      return self.B0.foo(x, y) + 3
             ~~~~~~~~~~~ <--- HERE

  File "<string>", line 3, in foo

    def foo(self, x, y):
      return self.A0.bar(x, y) + 2
             ~~~~~~~~~~~ <--- HERE

  File "<string>", line 3, in bar

    def bar(self, x, y):
      return x + y
             ~~~~~ <--- HERE
  )";
  ASSERT_THROWS_WITH_MESSAGE(lite_m.forward(inputs), error_pattern);
}
#endif // !defined(FB_XPLAT_BUILD)

namespace {
static auto reg =
    torch::class_<TorchBindLiteInterpreterTestStruct>(
        "_TorchScriptTesting",
        "_LiteInterpreterTest")
        .def(torch::init<>())
        .def("get", &TorchBindLiteInterpreterTestStruct::get)
        .def_pickle(
            // __getattr__
            [](const c10::intrusive_ptr<TorchBindLiteInterpreterTestStruct>&
                   self) -> int64_t { return 0; },
            // __setattr__
            [](int64_t state) {
              return c10::make_intrusive<TorchBindLiteInterpreterTestStruct>();
            });

} // namespace

TEST(LiteInterpreterTest, OperatorCacheDifferentiatesDefaultArgs) {
  // Create 3 methods:
  //
  // 1. forward() returns a tensor with dtype=torch.int64 (4)
  // 2. forward2() returns a tensor with dtype=torch.float32 (6)
  // 3. forward3() returns a tensor with dtype=torch.float32 but
  //    the dtype is inferred by the input tensor's dtype
  //
  // If caching works correctly, then the result from the full-jit
  // module and the lite module will be the same. Otherwise, it
  // will be different if we don't correctly ignore the cache
  // entry for an operator that has a different number of
  // arguments.
  Module m("m");
  m.define(R"(
    def forward(self):
      ret1 = torch.new_empty(torch.zeros(10), [10], dtype=4)
      return ret1.fill_(25)
  )");
  m.define(R"(
    def forward2(self):
      ret1 = torch.new_empty(torch.zeros(10), [10], dtype=6)
      return ret1.fill_(32.0)
  )");
  m.define(R"(
    def forward3(self):
      ret1 = torch.new_empty(torch.zeros(10), [10])
      return ret1.fill_(12.0)
  )");

  std::vector<torch::jit::IValue> inputs;
  testLiteModuleCompareResultTensors(m, inputs, "forward");
  testLiteModuleCompareResultTensors(m, inputs, "forward2");
  testLiteModuleCompareResultTensors(m, inputs, "forward3");
}

TEST(RunTimeTest, RuntimeCall) {
  //     def call(x):
  //         return x + x
  //
  //     def forward(a):
  //         x = a + call(a)
  //         y = a + call(x)
  //         return y

  std::vector<IValue> instructionsCall{
      to_tuple({"STORE", 1, 0}),
      to_tuple({"LOAD", 1, 0}),
      to_tuple({"MOVE", 1, 0}),
      to_tuple({"LOADC", 0, 0}),
      to_tuple({"OP", 0, 0}),
      to_tuple({"RET", 0, 0}),
  };
  std::vector<IValue> instructionsFoo{
      to_tuple({"STORE", 1, 0}),
      to_tuple({"LOAD", 1, 0}),
      to_tuple({"LOAD", 1, 0}),
      to_tuple({"MOVE", 1, 0}),
      to_tuple({"CALL", 0, 0}),
      to_tuple({"LOADC", 0, 0}),
      to_tuple({"OP", 0, 0}),
      to_tuple({"CALL", 0, 0}),
      to_tuple({"LOADC", 0, 0}),
      to_tuple({"OP", 0, 0}),
      to_tuple({"RET", 0, 0}),
  };
  std::vector<IValue> operatorsFoo{
      to_tuple({"aten::add", "Tensor", 3}),
  };
  std::vector<IValue> constantsFoo{
      1,
  };
  std::vector<IValue> operatorsCall{
      to_tuple({"aten::add", "Tensor", 3}),
  };
  std::vector<IValue> constantsCall{
      1,
  };
  int64_t model_version = caffe2::serialize::kProducedBytecodeVersion;

  auto foo = std::make_unique<mobile::Function>(c10::QualifiedName("foo"));
  c10::ivalue::TupleElements debug_handles_m_tuple;
  parseInstructions(
      "foo",
      std::move(*c10::ivalue::Tuple::create(instructionsFoo)).elements(),
      debug_handles_m_tuple,
      foo.get());
  parseOperators(
      std::move(*c10::ivalue::Tuple::create(operatorsFoo)).elements(),
      model_version,
      1,
      foo.get());
  parseConstants(
      std::move(*c10::ivalue::Tuple::create(constantsFoo)).elements(),
      foo.get());
  const size_t rsize = 5;
  parseRegisterSize(rsize, foo.get());

  auto call = std::make_unique<mobile::Function>(c10::QualifiedName("call"));
  parseInstructions(
      "call",
      std::move(*c10::ivalue::Tuple::create(instructionsCall)).elements(),
      debug_handles_m_tuple,
      call.get());
  parseOperators(
      std::move(*c10::ivalue::Tuple::create(operatorsCall)).elements(),
      model_version,
      1,
      call.get());
  parseConstants(
      std::move(*c10::ivalue::Tuple::create(constantsCall)).elements(),
      call.get());
  parseRegisterSize(rsize, call.get());

  foo->append_function(*call);

  std::vector<IValue> inputs{at::tensor(1)};
  foo->run(inputs);
  auto output = inputs[0];
  ASSERT_EQ(output, at::tensor(7));
}

TEST(LiteInterpreterTest, OperatorSize1) {
  Module m("m");
  m.define(R"(
    def forward(self, input: Tensor, scale:float):
      return torch.upsample_nearest2d(input, [1, 1], float(scale), float(scale))
  )");

  std::stringstream ss;
  m._save_for_mobile(ss);
  mobile::Module bc = _load_for_mobile(ss);
  const auto& func = bc.get_method("forward").function();
  ASSERT_EQ(
      func.get_code()->operator_input_sizes_.size(),
      func.get_code()->operators_.size());

  auto buff = save_mobile_module_to_bytes(bc);
  mobile::Module bc2 = parse_mobile_module(buff.data(), buff.size());
  const auto& func2 = bc.get_method("forward").function();
  ASSERT_EQ(
      func2.get_code()->operator_input_sizes_.size(),
      func2.get_code()->operators_.size());
}

TEST(LiteInterpreterTest, OperatorTest2) { // NOLINT (use =delete in gtest)
  const std::vector<std::string> test_programs{
      // test invoking a method with default parameter
      R"(
      def test_func(self, x, b : int = 4):
        return self.foo + x + b
      )",
      // inner method call with default parameter (gets inlined)
      R"(
      def add_with_default_arg(self, x, b : int = 4):
        return self.foo + x + b
      def test_func(self, x):
        return self.add_with_default_arg(x)  # invoke method w/ default arg
      )",
      // simple method call
      R"(
      def test_func(self, x):
        b = 4
        return self.foo + x + b
      )",
  };
  for (const auto& test_program : test_programs) {
    Module m("m");
    m.register_parameter("foo", torch::ones({}), false);
    m.define(test_program);

    std::stringstream ss;
    m._save_for_mobile(ss);
    mobile::Module bc = _load_for_mobile(ss);
    const auto& func = bc.get_method("test_func").function();
    ASSERT_EQ(
        func.get_code()->operator_input_sizes_.size(),
        func.get_code()->operators_.size());

    auto buff = save_mobile_module_to_bytes(bc);
    mobile::Module bc2 = parse_mobile_module(buff.data(), buff.size());
    const auto& func2 = bc.get_method("test_func").function();
    ASSERT_EQ(
        func2.get_code()->operator_input_sizes_.size(),
        func2.get_code()->operators_.size());
  }
}

#if !defined FB_XPLAT_BUILD
// The following test run in fbcode only
TEST(LiteInterpreterUpgraderTest, DivTensorV2) {
  std::string filePath(__FILE__);
  auto test_model_file = filePath.substr(0, filePath.find_last_of("/\\") + 1);
  test_model_file.append("upgrader_models/test_versioned_div_tensor_v2.ptl");
  mobile::Module m_module = _load_for_mobile(test_model_file);
  std::vector<IValue> inputs = {
      IValue(6 * torch::ones({1})), IValue(3 * torch::ones({1}))};
  auto actual_output = m_module.forward(inputs);
  auto expect_output = 2.0 * torch::ones({1});
  auto actual_output_list = actual_output.toTuple()->elements();
  ASSERT_TRUE(actual_output_list[0].toTensor().equal(expect_output));
}
#endif // !defined(FB_XPLAT_BUILD)

} // namespace jit
} // namespace torch
