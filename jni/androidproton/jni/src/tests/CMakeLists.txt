#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#

add_executable (c-object-tests object.c)
target_link_libraries (c-object-tests qpid-proton)
set_target_properties (
  c-object-tests
  PROPERTIES
  COMPILE_FLAGS "${COMPILE_WARNING_FLAGS} ${COMPILE_PLATFORM_FLAGS}"
  )
pn_c_files (object.c)

add_executable (c-message-tests message.c)
target_link_libraries (c-message-tests qpid-proton)
set_target_properties (
  c-message-tests
  PROPERTIES
  COMPILE_FLAGS "${COMPILE_WARNING_FLAGS} ${COMPILE_PLATFORM_FLAGS}"
  )
pn_c_files (message.c)

add_test (c-object-tests c-object-tests)
add_test (c-message-tests c-message-tests)
