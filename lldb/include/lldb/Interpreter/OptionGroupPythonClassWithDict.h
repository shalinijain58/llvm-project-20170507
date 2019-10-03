//===-- OptionGroupPythonClassWithDict.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_OptionGroupString_h_
#define liblldb_OptionGroupString_h_

#include "lldb/Interpreter/Options.h"
#include "lldb/Utility/StructuredData.h"

namespace lldb_private {

// Use this Option group if you have a python class that implements some
// Python extension point, and you pass a SBStructuredData to the class 
// __init__ method.  
// class_option specifies the class name
// the key and value options are read in in pairs, and a 
// StructuredData::Dictionary is constructed with those pairs.
class OptionGroupPythonClassWithDict : public OptionGroup {
public:
  OptionGroupPythonClassWithDict(const char *class_use,
                      int class_option = 'C',
                      int key_option = 'k', 
                      int value_option = 'v',
                      char *class_long_option = "python-class",
                      const char *key_long_option = "python-class-key",
                      const char *value_long_option = "python-class-value",
                      bool required = false);
                      
  ~OptionGroupPythonClassWithDict() override;

  llvm::ArrayRef<OptionDefinition> GetDefinitions() override {
    return llvm::ArrayRef<OptionDefinition>(m_option_definition);
  }

  Status SetOptionValue(uint32_t option_idx, llvm::StringRef option_value,
                        ExecutionContext *execution_context) override;
  Status SetOptionValue(uint32_t, const char *, ExecutionContext *) = delete;

  void OptionParsingStarting(ExecutionContext *execution_context) override;
  Status OptionParsingFinished(ExecutionContext *execution_context) override;
  
  const StructuredData::DictionarySP GetStructuredData() {
    return m_dict_sp;
  }
  const std::string &GetClassName() {
    return m_class_name;
  }

protected:
  std::string m_class_name;
  std::string m_current_key;
  StructuredData::DictionarySP m_dict_sp;
  std::string m_class_usage_text, m_key_usage_text, m_value_usage_text;
  OptionDefinition m_option_definition[3];
};

} // namespace lldb_private

#endif // liblldb_OptionGroupString_h_
