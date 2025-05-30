// Copyright 2025 Memgraph Ltd.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.txt; by using this file, you agree to be bound by the terms of the Business Source
// License, and you may not use this file except in compliance with the Business Source License.
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0, included in the file
// licenses/APL.txt.

#include <algorithm>
#include <iostream>
#include <optional>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "auth/auth.hpp"
#include "auth/crypto.hpp"
#include "auth/models.hpp"
#include "glue/auth_global.hpp"
#include "license/license.hpp"
#include "utils/cast.hpp"
#include "utils/file.hpp"

using namespace memgraph::auth;
namespace fs = std::filesystem;

DECLARE_string(password_encryption_algorithm);

class AuthWithStorage : public ::testing::Test {
 protected:
  void SetUp() override {
    memgraph::utils::EnsureDir(test_folder_);
    memgraph::license::global_license_checker.EnableTesting();
    auth.emplace(test_folder_ / ("unit_auth_test_" + std::to_string(static_cast<int>(getpid()))), auth_config);
  }

  void TearDown() override { fs::remove_all(test_folder_); }

  fs::path test_folder_{fs::temp_directory_path() / "MG_tests_unit_auth"};
  Auth::Config auth_config{};
  std::optional<Auth> auth{};
};

TEST_F(AuthWithStorage, AddRole) {
  ASSERT_TRUE(auth->AddRole("admin"));
  ASSERT_TRUE(auth->AddRole("user"));
  ASSERT_FALSE(auth->AddRole("admin"));
}

TEST_F(AuthWithStorage, RemoveRole) {
  ASSERT_TRUE(auth->AddRole("admin"));
  ASSERT_TRUE(auth->RemoveRole("admin"));
  ASSERT_FALSE(auth->HasUsers());
  ASSERT_FALSE(auth->RemoveUser("test2"));
  ASSERT_FALSE(auth->RemoveUser("test"));
  ASSERT_FALSE(auth->HasUsers());
}

TEST_F(AuthWithStorage, Authenticate) {
  ASSERT_FALSE(auth->HasUsers());

  auto user = auth->AddUser("test");
  ASSERT_NE(user, std::nullopt);
  ASSERT_TRUE(auth->HasUsers());

  ASSERT_TRUE(auth->Authenticate("test", "123"));
  ASSERT_TRUE(auth->Authenticate("test", ""));

  user->UpdatePassword("123");
  auth->SaveUser(*user);

  ASSERT_NE(auth->Authenticate("test", "123"), std::nullopt);

  ASSERT_EQ(auth->Authenticate("test", "456"), std::nullopt);
  ASSERT_NE(auth->Authenticate("test", "123"), std::nullopt);

  const auto bcrpyt_hash = HashPassword("456", PasswordHashAlgorithm::BCRYPT);
  user->UpdateHash(bcrpyt_hash);
  auth->SaveUser(*user);

  ASSERT_NE(auth->Authenticate("test", "456"), std::nullopt);

  ASSERT_EQ(auth->Authenticate("test", "123"), std::nullopt);
  ASSERT_NE(auth->Authenticate("test", "456"), std::nullopt);

  const auto sha256_hash = HashPassword("789", PasswordHashAlgorithm::SHA256);
  user->UpdateHash(sha256_hash);
  auth->SaveUser(*user);

  ASSERT_NE(auth->Authenticate("test", "789"), std::nullopt);

  ASSERT_EQ(auth->Authenticate("test", "456"), std::nullopt);
  ASSERT_NE(auth->Authenticate("test", "789"), std::nullopt);

  const auto sha256_mul_hash = HashPassword("012", PasswordHashAlgorithm::SHA256_MULTIPLE);
  user->UpdateHash(sha256_mul_hash);
  auth->SaveUser(*user);

  ASSERT_NE(auth->Authenticate("test", "012"), std::nullopt);

  ASSERT_EQ(auth->Authenticate("test", "567"), std::nullopt);
  ASSERT_NE(auth->Authenticate("test", "012"), std::nullopt);

  user->UpdatePassword();
  auth->SaveUser(*user);

  ASSERT_NE(auth->Authenticate("test", "123"), std::nullopt);
  ASSERT_NE(auth->Authenticate("test", "456"), std::nullopt);
  ASSERT_NE(auth->Authenticate("test", "789"), std::nullopt);
  ASSERT_NE(auth->Authenticate("test", "012"), std::nullopt);

  ASSERT_EQ(auth->Authenticate("nonexistant", "123"), std::nullopt);
}

TEST_F(AuthWithStorage, UserRolePermissions) {
  ASSERT_FALSE(auth->HasUsers());
  ASSERT_TRUE(auth->AddUser("test"));
  ASSERT_TRUE(auth->HasUsers());

  auto user = auth->GetUser("test");
  ASSERT_NE(user, std::nullopt);

  // Test initial user permissions.
  ASSERT_EQ(user->permissions().Has(Permission::MATCH), PermissionLevel::NEUTRAL);
  ASSERT_EQ(user->permissions().Has(Permission::CREATE), PermissionLevel::NEUTRAL);
  ASSERT_EQ(user->permissions().Has(Permission::MERGE), PermissionLevel::NEUTRAL);
  ASSERT_EQ(user->permissions().Has(Permission::DELETE), PermissionLevel::NEUTRAL);
  ASSERT_EQ(user->permissions(), user->GetPermissions());

  // Change one user permission.
  user->permissions().Grant(Permission::MATCH);

  // Check permissions.
  ASSERT_EQ(user->permissions().Has(Permission::MATCH), PermissionLevel::GRANT);
  ASSERT_EQ(user->permissions().Has(Permission::CREATE), PermissionLevel::NEUTRAL);
  ASSERT_EQ(user->permissions().Has(Permission::MERGE), PermissionLevel::NEUTRAL);
  ASSERT_EQ(user->permissions().Has(Permission::DELETE), PermissionLevel::NEUTRAL);
  ASSERT_EQ(user->permissions(), user->GetPermissions());

  // Create role.
  ASSERT_TRUE(auth->AddRole("admin"));
  auto role = auth->GetRole("admin");
  ASSERT_NE(role, std::nullopt);

  // Assign permissions to role and role to user.
  role->permissions().Grant(Permission::DELETE);
  user->SetRole(*role);

  // Check permissions.
  {
    auto permissions = user->GetPermissions();
    ASSERT_EQ(permissions.Has(Permission::MATCH), PermissionLevel::GRANT);
    ASSERT_EQ(permissions.Has(Permission::DELETE), PermissionLevel::GRANT);
    ASSERT_EQ(permissions.Has(Permission::CREATE), PermissionLevel::NEUTRAL);
    ASSERT_EQ(permissions.Has(Permission::MERGE), PermissionLevel::NEUTRAL);
  }

  // Add explicit deny to role.
  role->permissions().Deny(Permission::MATCH);
  user->SetRole(*role);

  // Check permissions.
  {
    auto permissions = user->GetPermissions();
    ASSERT_EQ(permissions.Has(Permission::MATCH), PermissionLevel::DENY);
    ASSERT_EQ(permissions.Has(Permission::DELETE), PermissionLevel::GRANT);
    ASSERT_EQ(permissions.Has(Permission::CREATE), PermissionLevel::NEUTRAL);
    ASSERT_EQ(permissions.Has(Permission::MERGE), PermissionLevel::NEUTRAL);
  }
}

#ifdef MG_ENTERPRISE
TEST_F(AuthWithStorage, UserRoleFineGrainedAccessHandler) {
  ASSERT_FALSE(auth->HasUsers());
  ASSERT_TRUE(auth->AddUser("test"));
  ASSERT_TRUE(auth->HasUsers());

  auto user = auth->GetUser("test");
  ASSERT_NE(user, std::nullopt);

  // Test initial user fine grained access permissions.
  ASSERT_EQ(user->fine_grained_access_handler().label_permissions(), FineGrainedAccessPermissions{});
  ASSERT_EQ(user->fine_grained_access_handler().edge_type_permissions(), FineGrainedAccessPermissions{});
  ASSERT_EQ(user->fine_grained_access_handler().label_permissions(), user->GetFineGrainedAccessLabelPermissions());
  ASSERT_EQ(user->fine_grained_access_handler().edge_type_permissions(),
            user->GetFineGrainedAccessEdgeTypePermissions());

  // Grant one label to user .
  user->fine_grained_access_handler().label_permissions().Grant("labelTest", FineGrainedPermission::CREATE_DELETE);
  // Grant one edge type to user .
  user->fine_grained_access_handler().edge_type_permissions().Grant("edgeTypeTest",
                                                                    FineGrainedPermission::CREATE_DELETE);

  // Check permissions.
  ASSERT_EQ(user->fine_grained_access_handler().label_permissions().Has("labelTest", FineGrainedPermission::READ),
            PermissionLevel::GRANT);
  ASSERT_EQ(
      user->fine_grained_access_handler().edge_type_permissions().Has("edgeTypeTest", FineGrainedPermission::READ),
      PermissionLevel::GRANT);
  ASSERT_EQ(user->fine_grained_access_handler().label_permissions(), user->GetFineGrainedAccessLabelPermissions());
  ASSERT_EQ(user->fine_grained_access_handler().edge_type_permissions(),
            user->GetFineGrainedAccessEdgeTypePermissions());

  // Check permissions.
  ASSERT_EQ(user->fine_grained_access_handler().label_permissions().Has("labelTest1", FineGrainedPermission::READ),
            PermissionLevel::DENY);
  ASSERT_EQ(
      user->fine_grained_access_handler().edge_type_permissions().Has("edgeTypeTest1", FineGrainedPermission::READ),
      PermissionLevel::DENY);
  ASSERT_EQ(user->fine_grained_access_handler().label_permissions(), user->GetFineGrainedAccessLabelPermissions());
  ASSERT_EQ(user->fine_grained_access_handler().edge_type_permissions(),
            user->GetFineGrainedAccessEdgeTypePermissions());

  // Create role.
  ASSERT_TRUE(auth->AddRole("admin"));
  auto role = auth->GetRole("admin");
  ASSERT_NE(role, std::nullopt);

  // Grant label and edge type to role and role to user.
  role->fine_grained_access_handler().label_permissions().Grant("roleLabelTest", FineGrainedPermission::CREATE_DELETE);
  role->fine_grained_access_handler().edge_type_permissions().Grant("roleEdgeTypeTest",
                                                                    FineGrainedPermission::CREATE_DELETE);
  user->SetRole(*role);

  // Check permissions.
  {
    ASSERT_EQ(user->GetFineGrainedAccessLabelPermissions().Has("roleLabelTest", FineGrainedPermission::READ),
              PermissionLevel::GRANT);
    ASSERT_EQ(user->GetFineGrainedAccessEdgeTypePermissions().Has("roleEdgeTypeTest", FineGrainedPermission::READ),
              PermissionLevel::GRANT);
  }

  user->SetRole(*role);

  // Check permissions.
  {
    ASSERT_EQ(user->GetFineGrainedAccessLabelPermissions().Has("roleLabelTest1", FineGrainedPermission::READ),
              PermissionLevel::DENY);
    ASSERT_EQ(user->GetFineGrainedAccessEdgeTypePermissions().Has("roleEdgeTypeTest1", FineGrainedPermission::READ),
              PermissionLevel::DENY);
  }
}
#endif

TEST_F(AuthWithStorage, RoleManipulations) {
  {
    auto user1 = auth->AddUser("user1");
    ASSERT_TRUE(user1);
    auto role1 = auth->AddRole("role1");
    ASSERT_TRUE(role1);
    user1->SetRole(*role1);
    auth->SaveUser(*user1);

    auto user2 = auth->AddUser("user2");
    ASSERT_TRUE(user2);
    auto role2 = auth->AddRole("role2");
    ASSERT_TRUE(role2);
    user2->SetRole(*role2);
    auth->SaveUser(*user2);
  }

  {
    auto user1 = auth->GetUser("user1");
    ASSERT_TRUE(user1);
    const auto *role1 = user1->role();
    ASSERT_NE(role1, nullptr);
    ASSERT_EQ(role1->rolename(), "role1");

    auto user2 = auth->GetUser("user2");
    ASSERT_TRUE(user2);
    const auto *role2 = user2->role();
    ASSERT_NE(role2, nullptr);
    ASSERT_EQ(role2->rolename(), "role2");
  }

  ASSERT_TRUE(auth->RemoveRole("role1"));

  {
    auto user1 = auth->GetUser("user1");
    ASSERT_TRUE(user1);
    const auto *role = user1->role();
    ASSERT_EQ(role, nullptr);

    auto user2 = auth->GetUser("user2");
    ASSERT_TRUE(user2);
    const auto *role2 = user2->role();
    ASSERT_NE(role2, nullptr);
    ASSERT_EQ(role2->rolename(), "role2");
  }

  {
    auto role1 = auth->AddRole("role1");
    ASSERT_TRUE(role1);
  }

  {
    auto user1 = auth->GetUser("user1");
    ASSERT_TRUE(user1);
    const auto *role1 = user1->role();
    ASSERT_EQ(role1, nullptr);

    auto user2 = auth->GetUser("user2");
    ASSERT_TRUE(user2);
    const auto *role2 = user2->role();
    ASSERT_NE(role2, nullptr);
    ASSERT_EQ(role2->rolename(), "role2");
  }

  {
    const auto all = auth->AllUsernames();
    for (const auto &user : all) std::cout << user << std::endl;
    auto users = auth->AllUsers();
    std::sort(users.begin(), users.end(), [](const User &a, const User &b) { return a.username() < b.username(); });
    ASSERT_EQ(users.size(), 2);
    ASSERT_EQ(users[0].username(), "user1");
    ASSERT_EQ(users[1].username(), "user2");
  }

  {
    auto roles = auth->AllRoles();
    std::sort(roles.begin(), roles.end(), [](const Role &a, const Role &b) { return a.rolename() < b.rolename(); });
    ASSERT_EQ(roles.size(), 2);
    ASSERT_EQ(roles[0].rolename(), "role1");
    ASSERT_EQ(roles[1].rolename(), "role2");
  }

  {
    auto users = auth->AllUsersForRole("role2");
    ASSERT_EQ(users.size(), 1);
    ASSERT_EQ(users[0].username(), "user2");
  }
}

TEST_F(AuthWithStorage, UserRoleLinkUnlink) {
  {
    auto user = auth->AddUser("user");
    ASSERT_TRUE(user);
    auto role = auth->AddRole("role");
    ASSERT_TRUE(role);
    user->SetRole(*role);
    auth->SaveUser(*user);
  }

  {
    auto user = auth->GetUser("user");
    ASSERT_TRUE(user);
    const auto *role = user->role();
    ASSERT_NE(role, nullptr);
    ASSERT_EQ(role->rolename(), "role");
  }

  {
    auto user = auth->GetUser("user");
    ASSERT_TRUE(user);
    user->ClearRole();
    auth->SaveUser(*user);
  }

  {
    auto user = auth->GetUser("user");
    ASSERT_TRUE(user);
    ASSERT_EQ(user->role(), nullptr);
  }
}

TEST_F(AuthWithStorage, UserPasswordCreation) {
  {
    auto user = auth->AddUser("test");
    ASSERT_TRUE(user);
    ASSERT_TRUE(auth->Authenticate("test", "123"));
    ASSERT_TRUE(auth->Authenticate("test", ""));
    ASSERT_TRUE(auth->RemoveUser(user->username()));
  }

  {
    auto user = auth->AddUser("test", "123");
    ASSERT_TRUE(user);
    ASSERT_TRUE(auth->Authenticate("test", "123"));
    ASSERT_FALSE(auth->Authenticate("test", "456"));
    ASSERT_TRUE(auth->RemoveUser(user->username()));
  }

  {
    auto user =
        auth->AddUser("test", "sha256:d74ff0ee8da3b9806b18c877dbf29bbde50b5bd8e4dad7a3a725000feb82e8f1" /* pass */);
    ASSERT_TRUE(user);
    ASSERT_TRUE(auth->Authenticate("test", "pass"));
    ASSERT_FALSE(auth->Authenticate("test", "word"));
    ASSERT_TRUE(auth->RemoveUser(user->username()));
  }

  {
    auto user = auth->AddUser("test", "bcrypt:$2a$12$laGNZfDIHu3t6jGr4Xm9i.siwQ78xfEb2VgXNqNGBV8FEbpHgNiQS" /* pass */);
    ASSERT_TRUE(user);
    ASSERT_TRUE(auth->Authenticate("test", "pass"));
    ASSERT_FALSE(auth->Authenticate("test", "word"));
    ASSERT_TRUE(auth->RemoveUser(user->username()));
  }
}

TEST_F(AuthWithStorage, PasswordStrength) {
  const std::string kWeakRegex = ".+";
  // https://stackoverflow.com/questions/5142103/regex-to-validate-password-strength
  const std::string kStrongRegex =
      "^(?=.*[A-Z].*[A-Z])(?=.*[!@#$&*])(?=.*[0-9].*[0-9])(?=.*[a-z].*[a-z].*["
      "a-z]).{8,}$";

  const std::string kWeakPassword = "weak";
  const std::string kAlmostStrongPassword = "ThisPasswordMeetsAllButOneCriterion1234";
  const std::string kStrongPassword = "ThisIsAVeryStrongPassword123$";

  {
    auth.reset();
    auth.emplace(test_folder_ / ("unit_auth_test_" + std::to_string(static_cast<int>(getpid()))),
                 Auth::Config{std::string{memgraph::glue::kDefaultUserRoleRegex}, kWeakRegex, true});
    auto user = auth->AddUser("user1");
    ASSERT_TRUE(user);
    ASSERT_NO_THROW(auth->UpdatePassword(*user, std::nullopt));
    ASSERT_NO_THROW(auth->UpdatePassword(*user, kWeakPassword));
    ASSERT_NO_THROW(auth->UpdatePassword(*user, kAlmostStrongPassword));
    ASSERT_NO_THROW(auth->UpdatePassword(*user, kStrongPassword));
  }

  {
    auth.reset();
    auth.emplace(test_folder_ / ("unit_auth_test_" + std::to_string(static_cast<int>(getpid()))),
                 Auth::Config{std::string{memgraph::glue::kDefaultUserRoleRegex}, kWeakRegex, false});
    ASSERT_THROW(auth->AddUser("user2", std::nullopt), AuthException);
    auto user = auth->AddUser("user2", kWeakPassword);
    ASSERT_TRUE(user);
    ASSERT_NO_THROW(auth->UpdatePassword(*user, kWeakPassword));
    ASSERT_NO_THROW(auth->UpdatePassword(*user, kAlmostStrongPassword));
    ASSERT_NO_THROW(auth->UpdatePassword(*user, kStrongPassword));
  }

  {
    auth.reset();
    auth.emplace(test_folder_ / ("unit_auth_test_" + std::to_string(static_cast<int>(getpid()))),
                 Auth::Config{std::string{memgraph::glue::kDefaultUserRoleRegex}, kStrongRegex, true});
    auto user = auth->AddUser("user3");
    ASSERT_TRUE(user);
    ASSERT_NO_THROW(auth->UpdatePassword(*user, std::nullopt));
    ASSERT_THROW(auth->UpdatePassword(*user, kWeakPassword), AuthException);
    ASSERT_THROW(auth->UpdatePassword(*user, kAlmostStrongPassword), AuthException);
    ASSERT_NO_THROW(auth->UpdatePassword(*user, kStrongPassword));
  }

  {
    auth.reset();
    auth.emplace(test_folder_ / ("unit_auth_test_" + std::to_string(static_cast<int>(getpid()))),
                 Auth::Config{std::string{memgraph::glue::kDefaultUserRoleRegex}, kStrongRegex, false});
    ASSERT_THROW(auth->AddUser("user4", std::nullopt);, AuthException);
    ASSERT_THROW(auth->AddUser("user4", kWeakPassword);, AuthException);
    ASSERT_THROW(auth->AddUser("user4", kAlmostStrongPassword);, AuthException);
    auto user = auth->AddUser("user4", kStrongPassword);
    ASSERT_TRUE(user);
    ASSERT_NO_THROW(auth->UpdatePassword(*user, kStrongPassword));
  }
}

TEST(AuthWithoutStorage, Permissions) {
  Permissions permissions;
  ASSERT_EQ(permissions.grants(), 0);
  ASSERT_EQ(permissions.denies(), 0);

  permissions.Grant(Permission::MATCH);
  ASSERT_EQ(permissions.Has(Permission::MATCH), PermissionLevel::GRANT);
  ASSERT_EQ(permissions.grants(), memgraph::utils::UnderlyingCast(Permission::MATCH));
  ASSERT_EQ(permissions.denies(), 0);

  permissions.Revoke(Permission::MATCH);
  ASSERT_EQ(permissions.Has(Permission::MATCH), PermissionLevel::NEUTRAL);
  ASSERT_EQ(permissions.grants(), 0);
  ASSERT_EQ(permissions.denies(), 0);

  permissions.Deny(Permission::MATCH);
  ASSERT_EQ(permissions.Has(Permission::MATCH), PermissionLevel::DENY);
  ASSERT_EQ(permissions.denies(), memgraph::utils::UnderlyingCast(Permission::MATCH));
  ASSERT_EQ(permissions.grants(), 0);

  permissions.Grant(Permission::MATCH);
  ASSERT_EQ(permissions.Has(Permission::MATCH), PermissionLevel::GRANT);
  ASSERT_EQ(permissions.grants(), memgraph::utils::UnderlyingCast(Permission::MATCH));
  ASSERT_EQ(permissions.denies(), 0);

  permissions.Deny(Permission::CREATE);
  ASSERT_EQ(permissions.Has(Permission::MATCH), PermissionLevel::GRANT);
  ASSERT_EQ(permissions.Has(Permission::CREATE), PermissionLevel::DENY);
  ASSERT_EQ(permissions.Has(Permission::MERGE), PermissionLevel::NEUTRAL);
  ASSERT_EQ(permissions.grants(), memgraph::utils::UnderlyingCast(Permission::MATCH));
  ASSERT_EQ(permissions.denies(), memgraph::utils::UnderlyingCast(Permission::CREATE));

  permissions.Grant(Permission::DELETE);
  ASSERT_EQ(permissions.Has(Permission::MATCH), PermissionLevel::GRANT);
  ASSERT_EQ(permissions.Has(Permission::CREATE), PermissionLevel::DENY);
  ASSERT_EQ(permissions.Has(Permission::MERGE), PermissionLevel::NEUTRAL);
  ASSERT_EQ(permissions.Has(Permission::DELETE), PermissionLevel::GRANT);
  ASSERT_EQ(permissions.grants(),
            memgraph::utils::UnderlyingCast(Permission::MATCH) | memgraph::utils::UnderlyingCast(Permission::DELETE));
  ASSERT_EQ(permissions.denies(), memgraph::utils::UnderlyingCast(Permission::CREATE));

  permissions.Revoke(Permission::DELETE);
  ASSERT_EQ(permissions.Has(Permission::MATCH), PermissionLevel::GRANT);
  ASSERT_EQ(permissions.Has(Permission::CREATE), PermissionLevel::DENY);
  ASSERT_EQ(permissions.Has(Permission::MERGE), PermissionLevel::NEUTRAL);
  ASSERT_EQ(permissions.Has(Permission::DELETE), PermissionLevel::NEUTRAL);
  ASSERT_EQ(permissions.grants(), memgraph::utils::UnderlyingCast(Permission::MATCH));
  ASSERT_EQ(permissions.denies(), memgraph::utils::UnderlyingCast(Permission::CREATE));
}

TEST(AuthWithoutStorage, PermissionsMaskTest) {
  Permissions p1(0, 0);
  ASSERT_EQ(p1.grants(), 0);
  ASSERT_EQ(p1.denies(), 0);

  Permissions p2(1, 0);
  ASSERT_EQ(p2.grants(), 1);
  ASSERT_EQ(p2.denies(), 0);

  Permissions p3(1, 1);
  ASSERT_EQ(p3.grants(), 0);
  ASSERT_EQ(p3.denies(), 1);

  Permissions p4(3, 2);
  ASSERT_EQ(p4.grants(), 1);
  ASSERT_EQ(p4.denies(), 2);
}

#ifdef MG_ENTERPRISE
TEST(AuthWithoutStorage, FineGrainedAccessPermissions) {
  const std::string any_label = "AnyString";
  const std::string check_label = "Label";
  const std::string non_check_label = "OtherLabel";
  const std::string asterisk = "*";

  {
    FineGrainedAccessPermissions fga_permissions1, fga_permissions2;
    ASSERT_TRUE(fga_permissions1 == fga_permissions2);
  }

  {
    FineGrainedAccessPermissions fga_permissions;
    ASSERT_TRUE(fga_permissions.GetPermissions().empty());
    ASSERT_EQ(fga_permissions.GetGlobalPermission(), std::nullopt);

    ASSERT_EQ(fga_permissions.Has(any_label, FineGrainedPermission::CREATE_DELETE), PermissionLevel::DENY);
    ASSERT_EQ(fga_permissions.Has(any_label, FineGrainedPermission::UPDATE), PermissionLevel::DENY);
    ASSERT_EQ(fga_permissions.Has(any_label, FineGrainedPermission::READ), PermissionLevel::DENY);
  }

  {
    FineGrainedAccessPermissions fga_permissions;
    fga_permissions.Grant(any_label, FineGrainedPermission::CREATE_DELETE);

    ASSERT_EQ(fga_permissions.GetGlobalPermission(), std::nullopt);
    ASSERT_FALSE(fga_permissions.GetPermissions().empty());
  }

  {
    FineGrainedAccessPermissions fga_permissions;
    fga_permissions.Grant(asterisk, FineGrainedPermission::CREATE_DELETE);

    ASSERT_EQ(fga_permissions.GetGlobalPermission(), kLabelPermissionAll);
    ASSERT_TRUE(fga_permissions.GetPermissions().empty());
  }

  {
    FineGrainedAccessPermissions fga_permissions;
    fga_permissions.Grant(asterisk, FineGrainedPermission::CREATE_DELETE);
    fga_permissions.Revoke(any_label);

    ASSERT_EQ(fga_permissions.GetGlobalPermission(), kLabelPermissionAll);
    ASSERT_TRUE(fga_permissions.GetPermissions().empty());
  }

  {
    FineGrainedAccessPermissions fga_permissions;
    fga_permissions.Grant(any_label, FineGrainedPermission::CREATE_DELETE);
    fga_permissions.Revoke(any_label);

    ASSERT_EQ(fga_permissions.GetGlobalPermission(), std::nullopt);
    ASSERT_TRUE(fga_permissions.GetPermissions().empty());
  }

  {
    FineGrainedAccessPermissions fga_permissions;
    fga_permissions.Grant(any_label, FineGrainedPermission::CREATE_DELETE);
    fga_permissions.Revoke(asterisk);

    ASSERT_EQ(fga_permissions.GetGlobalPermission(), std::nullopt);
    ASSERT_TRUE(fga_permissions.GetPermissions().empty());
  }

  {
    FineGrainedAccessPermissions fga_permissions;
    fga_permissions.Grant(asterisk, FineGrainedPermission::CREATE_DELETE);

    ASSERT_EQ(fga_permissions.Has(any_label, FineGrainedPermission::CREATE_DELETE), PermissionLevel::GRANT);
    ASSERT_EQ(fga_permissions.Has(any_label, FineGrainedPermission::UPDATE), PermissionLevel::GRANT);
    ASSERT_EQ(fga_permissions.Has(any_label, FineGrainedPermission::READ), PermissionLevel::GRANT);
  }

  {
    FineGrainedAccessPermissions fga_permissions;
    fga_permissions.Grant(asterisk, FineGrainedPermission::UPDATE);

    ASSERT_EQ(fga_permissions.Has(any_label, FineGrainedPermission::CREATE_DELETE), PermissionLevel::DENY);
    ASSERT_EQ(fga_permissions.Has(any_label, FineGrainedPermission::UPDATE), PermissionLevel::GRANT);
    ASSERT_EQ(fga_permissions.Has(any_label, FineGrainedPermission::READ), PermissionLevel::GRANT);
  }

  {
    FineGrainedAccessPermissions fga_permissions;
    fga_permissions.Grant(asterisk, FineGrainedPermission::CREATE_DELETE);

    ASSERT_EQ(fga_permissions.Has(any_label, FineGrainedPermission::CREATE_DELETE), PermissionLevel::GRANT);
    ASSERT_EQ(fga_permissions.Has(any_label, FineGrainedPermission::UPDATE), PermissionLevel::GRANT);
    ASSERT_EQ(fga_permissions.Has(any_label, FineGrainedPermission::READ), PermissionLevel::GRANT);
  }

  {
    FineGrainedAccessPermissions fga_permissions;
    fga_permissions.Grant(asterisk, FineGrainedPermission::READ);
    fga_permissions.Grant(check_label, FineGrainedPermission::UPDATE);

    ASSERT_EQ(fga_permissions.Has(check_label, FineGrainedPermission::CREATE_DELETE), PermissionLevel::DENY);
    ASSERT_EQ(fga_permissions.Has(check_label, FineGrainedPermission::UPDATE), PermissionLevel::GRANT);
    ASSERT_EQ(fga_permissions.Has(check_label, FineGrainedPermission::READ), PermissionLevel::GRANT);
    ASSERT_EQ(fga_permissions.Has(non_check_label, FineGrainedPermission::CREATE_DELETE), PermissionLevel::DENY);
    ASSERT_EQ(fga_permissions.Has(non_check_label, FineGrainedPermission::UPDATE), PermissionLevel::DENY);
    ASSERT_EQ(fga_permissions.Has(non_check_label, FineGrainedPermission::READ), PermissionLevel::GRANT);
  }
}

TEST_F(AuthWithStorage, FineGrainedAccessCheckerMerge) {
  const auto *any_label = "AnyString";
  const auto *check_label = "Label";
  const auto *asterisk = "*";

  {
    FineGrainedAccessPermissions fga_permissions1, fga_permissions2;
    fga_permissions1.Grant(asterisk, FineGrainedPermission::READ);

    auto fga_permissions3 = memgraph::auth::Merge(fga_permissions1, fga_permissions2);

    ASSERT_EQ(fga_permissions3.Has(any_label, FineGrainedPermission::CREATE_DELETE), PermissionLevel::DENY);
    ASSERT_EQ(fga_permissions3.Has(any_label, FineGrainedPermission::UPDATE), PermissionLevel::DENY);
    ASSERT_EQ(fga_permissions3.Has(any_label, FineGrainedPermission::READ), PermissionLevel::GRANT);
  }

  {
    FineGrainedAccessPermissions fga_permissions1, fga_permissions2;
    fga_permissions2.Grant(asterisk, FineGrainedPermission::READ);

    auto fga_permissions3 = memgraph::auth::Merge(fga_permissions1, fga_permissions2);

    ASSERT_EQ(fga_permissions3.Has(any_label, FineGrainedPermission::CREATE_DELETE), PermissionLevel::DENY);
    ASSERT_EQ(fga_permissions3.Has(any_label, FineGrainedPermission::UPDATE), PermissionLevel::DENY);
    ASSERT_EQ(fga_permissions3.Has(any_label, FineGrainedPermission::READ), PermissionLevel::GRANT);
  }

  {
    FineGrainedAccessPermissions fga_permissions1, fga_permissions2;
    fga_permissions1.Grant(asterisk, FineGrainedPermission::READ);
    fga_permissions2.Grant(asterisk, FineGrainedPermission::UPDATE);

    auto fga_permissions3 = memgraph::auth::Merge(fga_permissions1, fga_permissions2);

    ASSERT_EQ(fga_permissions3.Has(any_label, FineGrainedPermission::CREATE_DELETE), PermissionLevel::DENY);
    ASSERT_EQ(fga_permissions3.Has(any_label, FineGrainedPermission::UPDATE), PermissionLevel::GRANT);
    ASSERT_EQ(fga_permissions3.Has(any_label, FineGrainedPermission::READ), PermissionLevel::GRANT);
  }

  {
    FineGrainedAccessPermissions fga_permissions1, fga_permissions2;
    fga_permissions1.Grant(asterisk, FineGrainedPermission::READ);
    fga_permissions1.Grant(check_label, FineGrainedPermission::UPDATE);
    fga_permissions2.Grant(asterisk, FineGrainedPermission::UPDATE);

    auto fga_permissions3 = memgraph::auth::Merge(fga_permissions1, fga_permissions2);

    ASSERT_EQ(fga_permissions3.Has(check_label, FineGrainedPermission::CREATE_DELETE), PermissionLevel::DENY);
    ASSERT_EQ(fga_permissions3.Has(check_label, FineGrainedPermission::UPDATE), PermissionLevel::GRANT);
    ASSERT_EQ(fga_permissions3.Has(check_label, FineGrainedPermission::READ), PermissionLevel::GRANT);
  }

  {
    FineGrainedAccessPermissions fga_permissions1, fga_permissions2;
    fga_permissions1.Grant(asterisk, FineGrainedPermission::READ);
    fga_permissions1.Grant(check_label, FineGrainedPermission::CREATE_DELETE);
    fga_permissions2.Grant(asterisk, FineGrainedPermission::UPDATE);
    fga_permissions2.Grant(check_label, FineGrainedPermission::READ);

    auto fga_permissions3 = memgraph::auth::Merge(fga_permissions1, fga_permissions2);

    ASSERT_EQ(fga_permissions3.Has(check_label, FineGrainedPermission::CREATE_DELETE), PermissionLevel::DENY);
    ASSERT_EQ(fga_permissions3.Has(check_label, FineGrainedPermission::UPDATE), PermissionLevel::DENY);
    ASSERT_EQ(fga_permissions3.Has(check_label, FineGrainedPermission::READ), PermissionLevel::GRANT);
  }
}
#endif

TEST(AuthWithoutStorage, UserSerializeDeserialize) {
  auto user = User("test");
  user.permissions().Grant(Permission::MATCH);
  user.permissions().Deny(Permission::MERGE);
  user.UpdatePassword("world");

  auto data = user.Serialize();

  auto output = User::Deserialize(data);
  ASSERT_EQ(user, output);
}

TEST(AuthWithoutStorage, UserSerializeDeserializeWithOutPassword) {
  auto user = User("test");
  user.permissions().Grant(Permission::MATCH);
  user.permissions().Deny(Permission::MERGE);

  auto data = user.Serialize();

  auto output = User::Deserialize(data);
  ASSERT_EQ(user, output);
}

TEST(AuthWithoutStorage, RoleSerializeDeserialize) {
  auto role = Role("test");
  role.permissions().Grant(Permission::MATCH);
  role.permissions().Deny(Permission::MERGE);

  auto data = role.Serialize();

  auto output = Role::Deserialize(data);
  ASSERT_EQ(role, output);
}

TEST_F(AuthWithStorage, UserWithRoleSerializeDeserialize) {
  auto role = auth->AddRole("role");
  ASSERT_TRUE(role);
  role->permissions().Grant(Permission::MATCH);
  role->permissions().Deny(Permission::MERGE);
  auth->SaveRole(*role);

  auto user = auth->AddUser("user");
  ASSERT_TRUE(user);
  user->permissions().Grant(Permission::MATCH);
  user->permissions().Deny(Permission::MERGE);
  user->UpdatePassword("world");
  user->SetRole(*role);
  auth->SaveUser(*user);

  auto new_user = auth->GetUser("user");
  ASSERT_TRUE(new_user);
  ASSERT_EQ(*user, *new_user);
}

TEST_F(AuthWithStorage, UserRoleUniqueName) {
  ASSERT_TRUE(auth->AddUser("user"));
  ASSERT_TRUE(auth->AddRole("role"));
  ASSERT_FALSE(auth->AddRole("user"));
  ASSERT_FALSE(auth->AddUser("role"));
}

TEST(AuthWithoutStorage, CaseInsensitivity) {
  {
    auto user1 = User("test");
    auto user2 = User("Test");
    ASSERT_EQ(user1, user2);
    ASSERT_EQ(user1.username(), user2.username());
    ASSERT_EQ(user1.username(), "test");
    ASSERT_EQ(user2.username(), "test");
  }
  {
    auto perms = Permissions();
    auto fine_grained_access_handler = FineGrainedAccessHandler();
    auto passwordHash = HashPassword("pw");
    auto user1 = User("test", passwordHash, perms, fine_grained_access_handler);
    auto user2 = User("Test", passwordHash, perms, fine_grained_access_handler);
    ASSERT_EQ(user1, user2);
    ASSERT_EQ(user1.username(), user2.username());
    ASSERT_EQ(user1.username(), "test");
    ASSERT_EQ(user2.username(), "test");
  }
  {
    auto role1 = Role("role");
    auto role2 = Role("Role");
    ASSERT_EQ(role1, role2);
    ASSERT_EQ(role1.rolename(), role2.rolename());
    ASSERT_EQ(role1.rolename(), "role");
    ASSERT_EQ(role2.rolename(), "role");
  }
  {
    auto perms = Permissions();
    auto fine_grained_access_handler = FineGrainedAccessHandler();
    auto role1 = Role("role", perms, fine_grained_access_handler);
    auto role2 = Role("Role", perms, fine_grained_access_handler);
    ASSERT_EQ(role1, role2);
    ASSERT_EQ(role1.rolename(), role2.rolename());
    ASSERT_EQ(role1.rolename(), "role");
    ASSERT_EQ(role2.rolename(), "role");
  }
}

TEST_F(AuthWithStorage, CaseInsensitivity) {
  // AddUser
  {
    auto user = auth->AddUser("Alice", "alice");
    ASSERT_TRUE(user);
    ASSERT_EQ(user->username(), "alice");
    ASSERT_FALSE(auth->AddUser("alice"));
    ASSERT_FALSE(auth->AddUser("alicE"));
  }
  {
    auto user = auth->AddUser("BoB", "bob");
    ASSERT_TRUE(user);
    ASSERT_EQ(user->username(), "bob");
    ASSERT_FALSE(auth->AddUser("bob"));
    ASSERT_FALSE(auth->AddUser("bOb"));
  }

  // Authenticate
  {
    auto user_or_role = auth->Authenticate("alice", "alice");
    ASSERT_TRUE(user_or_role);
    const auto &user = std::get<memgraph::auth::User>(*user_or_role);
    ASSERT_EQ(user.username(), "alice");
  }
  {
    auto user_or_role = auth->Authenticate("alICe", "alice");
    ASSERT_TRUE(user_or_role);
    const auto &user = std::get<memgraph::auth::User>(*user_or_role);
    ASSERT_EQ(user.username(), "alice");
  }

  // GetUser
  {
    auto user = auth->GetUser("alice");
    ASSERT_TRUE(user);
    ASSERT_EQ(user->username(), "alice");
  }
  {
    auto user = auth->GetUser("aLicE");
    ASSERT_TRUE(user);
    ASSERT_EQ(user->username(), "alice");
  }
  ASSERT_FALSE(auth->GetUser("carol"));

  // RemoveUser
  {
    auto user = auth->AddUser("caRol", "carol");
    ASSERT_TRUE(user);
    ASSERT_EQ(user->username(), "carol");
    ASSERT_TRUE(auth->RemoveUser("cAROl"));
    ASSERT_FALSE(auth->RemoveUser("carol"));
    ASSERT_FALSE(auth->GetUser("CAROL"));
  }

  // AllUsers
  {
    const auto all = auth->AllUsernames();
    for (const auto &user : all) std::cout << user << std::endl;
    auto users = auth->AllUsers();
    ASSERT_EQ(users.size(), 2);
    std::sort(users.begin(), users.end(), [](const auto &a, const auto &b) { return a.username() < b.username(); });
    ASSERT_EQ(users[0].username(), "alice");
    ASSERT_EQ(users[1].username(), "bob");
  }

  // AddRole
  {
    auto role = auth->AddRole("Moderator");
    ASSERT_TRUE(role);
    ASSERT_EQ(role->rolename(), "moderator");
    ASSERT_FALSE(auth->AddRole("moderator"));
    ASSERT_FALSE(auth->AddRole("MODERATOR"));
  }
  {
    auto role = auth->AddRole("adMIN");
    ASSERT_TRUE(role);
    ASSERT_EQ(role->rolename(), "admin");
    ASSERT_FALSE(auth->AddRole("Admin"));
    ASSERT_FALSE(auth->AddRole("ADMIn"));
  }
  ASSERT_FALSE(auth->AddRole("ALICE"));
  ASSERT_FALSE(auth->AddUser("ModeRAtor"));

  // GetRole
  {
    auto role = auth->GetRole("moderator");
    ASSERT_TRUE(role);
    ASSERT_EQ(role->rolename(), "moderator");
  }
  {
    auto role = auth->GetRole("MoDERATOR");
    ASSERT_TRUE(role);
    ASSERT_EQ(role->rolename(), "moderator");
  }
  ASSERT_FALSE(auth->GetRole("root"));

  // RemoveRole
  {
    auto role = auth->AddRole("RooT");
    ASSERT_TRUE(role);
    ASSERT_EQ(role->rolename(), "root");
    ASSERT_TRUE(auth->RemoveRole("rOOt"));
    ASSERT_FALSE(auth->RemoveRole("RoOt"));
    ASSERT_FALSE(auth->GetRole("RoOt"));
  }

  // AllRoles
  {
    auto roles = auth->AllRoles();
    ASSERT_EQ(roles.size(), 2);
    std::sort(roles.begin(), roles.end(), [](const auto &a, const auto &b) { return a.rolename() < b.rolename(); });
    ASSERT_EQ(roles[0].rolename(), "admin");
    ASSERT_EQ(roles[1].rolename(), "moderator");
  }

  // SaveRole
  {
    auto role = auth->GetRole("MODErator");
    ASSERT_TRUE(role);
    ASSERT_EQ(role->rolename(), "moderator");
    role->permissions().Grant(memgraph::auth::Permission::MATCH);
    auth->SaveRole(*role);
  }
  {
    auto role = auth->GetRole("modeRATOR");
    ASSERT_TRUE(role);
    ASSERT_EQ(role->rolename(), "moderator");
    ASSERT_EQ(role->permissions().Has(memgraph::auth::Permission::MATCH), memgraph::auth::PermissionLevel::GRANT);
  }

  // SaveUser
  {
    auto user = auth->GetUser("aLice");
    ASSERT_TRUE(user);
    ASSERT_EQ(user->username(), "alice");
    auto role = auth->GetRole("moderAtor");
    ASSERT_TRUE(role);
    ASSERT_EQ(role->rolename(), "moderator");
    user->SetRole(*role);
    auth->SaveUser(*user);
  }
  {
    auto user = auth->GetUser("aLIce");
    ASSERT_TRUE(user);
    ASSERT_EQ(user->username(), "alice");
    const auto *role = user->role();
    ASSERT_NE(role, nullptr);
    ASSERT_EQ(role->rolename(), "moderator");
  }

  // AllUsersForRole
  {
    auto carol = auth->AddUser("caROl");
    ASSERT_TRUE(carol);
    ASSERT_EQ(carol->username(), "carol");
    auto dave = auth->AddUser("daVe");
    ASSERT_TRUE(dave);
    ASSERT_EQ(dave->username(), "dave");
    auto admin = auth->GetRole("aDMin");
    ASSERT_TRUE(admin);
    ASSERT_EQ(admin->rolename(), "admin");
    carol->SetRole(*admin);
    auth->SaveUser(*carol);
    dave->SetRole(*admin);
    auth->SaveUser(*dave);
  }
  {
    auto users = auth->AllUsersForRole("modeRAtoR");
    ASSERT_EQ(users.size(), 1);
    ASSERT_EQ(users[0].username(), "alice");
  }
  {
    auto users = auth->AllUsersForRole("AdmiN");
    ASSERT_EQ(users.size(), 2);
    std::sort(users.begin(), users.end(), [](const auto &a, const auto &b) { return a.username() < b.username(); });
    ASSERT_EQ(users[0].username(), "carol");
    ASSERT_EQ(users[1].username(), "dave");
  }
}

TEST(AuthWithoutStorage, Crypto) {
  auto hash = HashPassword("hello");
  ASSERT_TRUE(hash.VerifyPassword("hello"));
  ASSERT_FALSE(hash.VerifyPassword("hello1"));
}

class AuthWithVariousEncryptionAlgorithms : public ::testing::Test {
 protected:
  void SetUp() override { SetHashAlgorithm("bcrypt"); }
};

TEST_F(AuthWithVariousEncryptionAlgorithms, VerifyPasswordDefault) {
  auto hash = HashPassword("hello");
  ASSERT_TRUE(hash.VerifyPassword("hello"));
  ASSERT_FALSE(hash.VerifyPassword("hello1"));
}

TEST_F(AuthWithVariousEncryptionAlgorithms, VerifyPasswordSHA256) {
  SetHashAlgorithm("sha256");
  auto hash = HashPassword("hello");
  ASSERT_TRUE(hash.VerifyPassword("hello"));
  ASSERT_FALSE(hash.VerifyPassword("hello1"));
}

TEST_F(AuthWithVariousEncryptionAlgorithms, VerifyPasswordSHA256_1024) {
  SetHashAlgorithm("sha256-multiple");
  auto hash = HashPassword("hello");
  ASSERT_TRUE(hash.VerifyPassword("hello"));
  ASSERT_FALSE(hash.VerifyPassword("hello1"));
}

TEST_F(AuthWithVariousEncryptionAlgorithms, SetEncryptionAlgorithmNonsenseThrow) {
  ASSERT_THROW(SetHashAlgorithm("abcd"), AuthException);
}

TEST_F(AuthWithVariousEncryptionAlgorithms, SetEncryptionAlgorithmEmptyThrow) {
  ASSERT_THROW(SetHashAlgorithm(""), AuthException);
}

class AuthWithStorageWithVariousEncryptionAlgorithms : public ::testing::Test {
 protected:
  void SetUp() override {
    memgraph::utils::EnsureDir(test_folder_);
    SetHashAlgorithm("bcrypt");

    memgraph::license::global_license_checker.EnableTesting();
  }

  void TearDown() override { fs::remove_all(test_folder_); }

  fs::path test_folder_{fs::temp_directory_path() / "MG_tests_unit_auth"};
  Auth::Config auth_config{};
  Auth auth{test_folder_ / ("unit_auth_test_" + std::to_string(static_cast<int>(getpid()))), auth_config};
};

TEST_F(AuthWithStorageWithVariousEncryptionAlgorithms, AddUserDefault) {
  auto user = auth.AddUser("Alice", "alice");
  ASSERT_TRUE(user);
  ASSERT_EQ(user->username(), "alice");
}

TEST_F(AuthWithStorageWithVariousEncryptionAlgorithms, AddUserSha256) {
  SetHashAlgorithm("sha256");
  auto user = auth.AddUser("Alice", "alice");
  ASSERT_TRUE(user);
  ASSERT_EQ(user->username(), "alice");
}

TEST_F(AuthWithStorageWithVariousEncryptionAlgorithms, AddUserSha256_1024) {
  SetHashAlgorithm("sha256-multiple");
  auto user = auth.AddUser("Alice", "alice");
  ASSERT_TRUE(user);
  ASSERT_EQ(user->username(), "alice");
}

TEST(Serialize, HashedPassword) {
  for (auto algo :
       {PasswordHashAlgorithm::BCRYPT, PasswordHashAlgorithm::SHA256, PasswordHashAlgorithm::SHA256_MULTIPLE}) {
    auto sut = HashPassword("password", algo);
    nlohmann::json j = sut;
    auto ret = j.get<HashedPassword>();
    ASSERT_EQ(sut, ret);
    ASSERT_TRUE(ret.VerifyPassword("password"));
  }
}

#ifdef MG_ENTERPRISE

TEST_F(AuthWithStorage, UserImpersonationWUser) {
  ASSERT_TRUE(auth->AddUser("admin"));
  ASSERT_TRUE(auth->AddUser("user"));
  ASSERT_TRUE(auth->AddUser("another_user"));

  // User has no permissions by deafult; add some
  auto user = auth->GetUser("user");
  auto another_user = auth->GetUser("another_user");
  ASSERT_TRUE(user);
  ASSERT_TRUE(another_user);

  // it's not enough to have permission, you need to specify who you are allowed to impersonate
  auto admin = auth->GetUser("admin");
  ASSERT_TRUE(admin);
  admin->permissions().Grant(Permission::IMPERSONATE_USER);
  ASSERT_FALSE(admin->CanImpersonate(*user));
  ASSERT_FALSE(admin->CanImpersonate(*another_user));

  // allow impersonation of only "another_user"
  admin->GrantUserImp({*another_user});
  ASSERT_FALSE(admin->CanImpersonate(*user));
  ASSERT_TRUE(admin->CanImpersonate(*another_user));

  // allow admin to impersonate anyone
  admin->GrantUserImp();
  ASSERT_TRUE(admin->CanImpersonate(*user));
  ASSERT_TRUE(admin->CanImpersonate(*another_user));

  // deny impersonation of only "user"
  admin->DenyUserImp({*user});
  ASSERT_FALSE(admin->CanImpersonate(*user));
  ASSERT_TRUE(admin->CanImpersonate(*another_user));

  // Reload user
  auth->SaveUser(*admin);
  admin = auth->GetUser("admin");
  ASSERT_TRUE(admin);
  // deny impersonation of only "user"
  admin->DenyUserImp({*user});
  ASSERT_FALSE(admin->CanImpersonate(*user));
  ASSERT_TRUE(admin->CanImpersonate(*another_user));
}

TEST_F(AuthWithStorage, UserImpersonationWRole) {
  ASSERT_TRUE(auth->AddRole("admin"));
  ASSERT_TRUE(auth->AddUser("user"));
  ASSERT_TRUE(auth->AddUser("another_user"));

  // User has no permissions by deafult; add some
  auto user = auth->GetUser("user");
  auto another_user = auth->GetUser("another_user");
  ASSERT_TRUE(user);
  ASSERT_TRUE(another_user);
  user->permissions().Grant(Permission::MATCH);

  // even the admin can impersonate by default
  auto admin = auth->GetRole("admin");
  ASSERT_TRUE(admin);
  admin->permissions().Grant(Permission::IMPERSONATE_USER);
  ASSERT_FALSE(admin->CanImpersonate(*user));
  ASSERT_FALSE(admin->CanImpersonate(*another_user));

  // allow impersonation of only "another_user"
  admin->GrantUserImp({*another_user});
  ASSERT_FALSE(admin->CanImpersonate(*user));
  ASSERT_TRUE(admin->CanImpersonate(*another_user));

  // allow admin to impersonate anyone
  admin->GrantUserImp();
  ASSERT_TRUE(admin->CanImpersonate(*user));
  ASSERT_TRUE(admin->CanImpersonate(*another_user));

  // deny impersonation of only "user"
  admin->DenyUserImp({*user});
  ASSERT_FALSE(admin->CanImpersonate(*user));
  ASSERT_TRUE(admin->CanImpersonate(*another_user));

  // user should have the same provoledges as its role
  user->SetRole(*admin);
  ASSERT_FALSE(user->CanImpersonate(*user));
  ASSERT_TRUE(user->CanImpersonate(*another_user));

  // Reload role
  auth->SaveRole(*admin);
  admin = auth->GetRole("admin");
  ASSERT_TRUE(admin);
  ASSERT_FALSE(user->CanImpersonate(*user));
  ASSERT_TRUE(user->CanImpersonate(*another_user));
}

TEST_F(AuthWithStorage, UserImpersonationWUserAndRole) {
  ASSERT_TRUE(auth->AddUser("admin"));
  ASSERT_TRUE(auth->AddRole("admin_role"));
  ASSERT_TRUE(auth->AddUser("user"));
  ASSERT_TRUE(auth->AddUser("another_user"));

  // User has no permissions by deafult; add some
  auto user = auth->GetUser("user");
  auto another_user = auth->GetUser("another_user");
  ASSERT_TRUE(user);
  ASSERT_TRUE(another_user);
  user->permissions().Grant(Permission::MATCH);

  // even the admin can impersonate by default
  auto admin_role = auth->GetRole("admin_role");
  ASSERT_TRUE(admin_role);
  auto admin = auth->GetUser("admin");
  ASSERT_TRUE(admin);
  admin->permissions().Grant(Permission::IMPERSONATE_USER);
  admin->SetRole(*admin_role);
  ASSERT_FALSE(admin->CanImpersonate(*user));
  ASSERT_FALSE(admin->CanImpersonate(*another_user));

  // allow impersonation of only "another_user"
  admin->GrantUserImp({*another_user});
  ASSERT_FALSE(admin->CanImpersonate(*user));
  ASSERT_TRUE(admin->CanImpersonate(*another_user));

  // allow role to impersonate "user"
  admin_role->GrantUserImp({*user});
  admin->SetRole(*admin_role);  // update role
  ASSERT_TRUE(admin->CanImpersonate(*user));
  ASSERT_TRUE(admin->CanImpersonate(*another_user));

  // allow admin to impersonate anyone
  admin->GrantUserImp();
  ASSERT_TRUE(admin->CanImpersonate(*user));
  ASSERT_TRUE(admin->CanImpersonate(*another_user));

  // deny role to impersonate "user"
  admin_role->DenyUserImp({*user});
  admin->SetRole(*admin_role);  // update role
  ASSERT_FALSE(admin->CanImpersonate(*user));
  ASSERT_TRUE(admin->CanImpersonate(*another_user));

  // allow role to impersonate anyone
  admin_role->GrantUserImp();
  admin->SetRole(*admin_role);  // update role
  ASSERT_TRUE(admin->CanImpersonate(*user));
  ASSERT_TRUE(admin->CanImpersonate(*another_user));

  // deny admin to impersonate "another_user"
  admin->DenyUserImp({*another_user});
  ASSERT_TRUE(admin->CanImpersonate(*user));
  ASSERT_FALSE(admin->CanImpersonate(*another_user));
}

#endif
