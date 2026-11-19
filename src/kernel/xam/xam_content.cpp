/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/cvar.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ppc/function.h>
#include <rex/ppc/types.h>
#include <rex/string/util.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xtypes.h>

#include <string>

REXCVAR_DEFINE_UINT32(license_mask, 0, "Kernel", "Set license mask for activated content");

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

ppc_u32_result_t XamContentGetLicenseMask_entry(ppc_pu32_t mask_ptr, ppc_pvoid_t overlapped_ptr) {
  // Each bit in the mask represents a granted license. Available licenses
  // seems to vary from game to game, but most appear to use bit 0 to indicate
  // if the game is purchased or not.
  *mask_ptr = REXCVAR_GET(license_mask);
  REXKRNL_INFO("XamContentGetLicenseMask: mask={:#x}", uint32_t(*mask_ptr));

  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(),
                                                    X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  } else {
    return X_ERROR_SUCCESS;
  }
}

ppc_u32_result_t XamContentResolve_entry(ppc_u32_t user_index, ppc_pvoid_t content_data_ptr,
                                         ppc_pvoid_t buffer_ptr, ppc_u32_t buffer_size,
                                         ppc_u32_t unk1, ppc_u32_t unk2, ppc_u32_t unk3) {
  auto content_data = content_data_ptr.as<XCONTENT_DATA*>();

  // Result of buffer_ptr is sent to RtlInitAnsiString.
  // buffer_size is usually 260 (max path).
  // Games expect zero if resolve was successful.
  REXKRNL_INFO("XamContentResolve: user_index={} buffer_size={} result={:#x}",
               uint32_t(user_index), uint32_t(buffer_size), uint32_t(X_ERROR_NOT_FOUND));
  assert_always();
  REXKRNL_WARN("XamContentResolve unimplemented!");
  return X_ERROR_NOT_FOUND;
}

// https://github.com/MrColdbird/gameservice/blob/master/ContentManager.cpp
// https://github.com/LestaD/SourceEngine2007/blob/master/se2007/engine/xboxsystem.cpp#L499
ppc_u32_result_t XamContentCreateEnumerator_entry(ppc_u32_t user_index, ppc_u32_t device_id,
                                                  ppc_u32_t content_type, ppc_u32_t content_flags,
                                                  ppc_u32_t items_per_enumerate,
                                                  ppc_pu32_t buffer_size_ptr,
                                                  ppc_pu32_t handle_out) {
  assert_not_null(handle_out);

  auto device_info = device_id == 0 ? nullptr : GetDummyDeviceInfo(device_id);
  if ((device_id && device_info == nullptr) || !handle_out) {
    if (buffer_size_ptr) {
      *buffer_size_ptr = 0;
    }
    return X_E_INVALIDARG;
  }

  if (buffer_size_ptr) {
    *buffer_size_ptr = sizeof(XCONTENT_DATA) * items_per_enumerate;
  }

  uint64_t xuid = REX_KERNEL_STATE()->user_profile()->xuid();

  auto e = make_object<XStaticEnumerator<XCONTENT_DATA>>(REX_KERNEL_STATE(), items_per_enumerate);
  auto result = e->Initialize(0xFF, 0xFE, 0x20005, 0x20007, 0);
  if (XFAILED(result)) {
    return result;
  }

  if (!device_info || device_info->device_id == DummyDeviceId::HDD) {
    // Enumerate user-specific content
    auto content_datas = REX_KERNEL_STATE()->content_manager()->ListContent(
        static_cast<uint32_t>(DummyDeviceId::HDD), xuid, XContentType(uint32_t(content_type)));
    for (const auto& content_data : content_datas) {
      auto item = e->AppendItem();
      *item = content_data;
    }

    // Also enumerate common content (xuid=0)
    if (xuid != 0) {
      auto common_datas = REX_KERNEL_STATE()->content_manager()->ListContent(
          static_cast<uint32_t>(DummyDeviceId::HDD), 0, XContentType(uint32_t(content_type)));
      for (const auto& content_data : common_datas) {
        auto item = e->AppendItem();
        *item = content_data;
      }
    }
  }

  if (!device_info || device_info->device_id == DummyDeviceId::ODD) {
    // TODO(gibbed): disc drive content
  }

  REXKRNL_DEBUG("XamContentCreateEnumerator: added {} items to enumerator", e->item_count());

  *handle_out = e->handle();
  return X_ERROR_SUCCESS;
}

enum class kDispositionState : uint32_t { Unknown = 0, Create = 1, Open = 2 };

ppc_u32_result_t xeXamContentCreate(const char* api_name, ppc_u32_t user_index,
                                    ppc_pchar_t root_name,
                                    ppc_pvoid_t content_data_ptr, ppc_u32_t content_data_size,
                                    ppc_u32_t flags, ppc_pu32_t disposition_ptr,
                                    ppc_pu32_t license_mask_ptr, ppc_u32_t cache_size,
                                    ppc_u64_t content_size, ppc_pvoid_t overlapped_ptr) {
  uint64_t xuid = REX_KERNEL_STATE()->user_profile()->xuid();

  XCONTENT_AGGREGATE_DATA content_data;
  if (content_data_size == sizeof(XCONTENT_DATA)) {
    content_data = *content_data_ptr.as<XCONTENT_DATA*>();
  } else if (content_data_size == sizeof(XCONTENT_AGGREGATE_DATA)) {
    content_data = *content_data_ptr.as<XCONTENT_AGGREGATE_DATA*>();
  } else {
    const auto root = root_name ? root_name.value() : std::string_view("<null>");
    REXKRNL_INFO("{}: root='{}' content_type=<unavailable> file_name=<unavailable> flags={:#x} "
                 "disposition=unknown result={:#x} license=0",
                 api_name, root, uint32_t(flags), uint32_t(X_ERROR_INVALID_PARAMETER));
    assert_always();
    return X_ERROR_INVALID_PARAMETER;
  }

  if (content_data.content_type == XContentType::kMarketplaceContent) {
    xuid = 0;
  }

  auto content_manager = REX_KERNEL_STATE()->content_manager();

  if (overlapped_ptr && disposition_ptr) {
    *disposition_ptr = 0;
  }

  auto run = [content_manager, xuid, api_name, root_name = std::string(root_name.value()),
              flags_value = static_cast<uint32_t>(flags), content_data, disposition_ptr,
              license_mask_ptr](uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    X_RESULT result = X_ERROR_INVALID_PARAMETER;
    kDispositionState disposition = kDispositionState::Unknown;
    switch (flags_value & 0xF) {
      case 1:  // CREATE_NEW
               // Fail if exists.
        if (content_manager->ContentExists(xuid, content_data)) {
          result = X_ERROR_ALREADY_EXISTS;
        } else {
          disposition = kDispositionState::Create;
        }
        break;
      case 2:  // CREATE_ALWAYS
               // Overwrite existing, if any.
        // Close any existing mount under this root name first.
        // Games may reuse the same root without explicitly closing.
        content_manager->CloseContent(root_name);
        if (content_manager->ContentExists(xuid, content_data)) {
          content_manager->DeleteContent(xuid, content_data);
        }
        // Check filesystem state after deletion attempt to decide
        // whether to create fresh or open existing.
        if (content_manager->ContentExists(xuid, content_data)) {
          disposition = kDispositionState::Open;
        } else {
          disposition = kDispositionState::Create;
        }
        break;
      case 3:  // OPEN_EXISTING
               // Open only if exists.
        if (!content_manager->ContentExists(xuid, content_data)) {
          result = X_ERROR_PATH_NOT_FOUND;
        } else {
          disposition = kDispositionState::Open;
        }
        break;
      case 4:  // OPEN_ALWAYS
               // Create if needed.
        if (!content_manager->ContentExists(xuid, content_data)) {
          disposition = kDispositionState::Create;
        } else {
          disposition = kDispositionState::Open;
        }
        break;
      case 5:  // TRUNCATE_EXISTING
               // Fail if doesn't exist, if does exist delete and recreate.
        if (!content_manager->ContentExists(xuid, content_data)) {
          result = X_ERROR_PATH_NOT_FOUND;
        } else {
          content_manager->CloseContent(root_name);
          content_manager->DeleteContent(xuid, content_data);
          if (content_manager->ContentExists(xuid, content_data)) {
            disposition = kDispositionState::Open;
          } else {
            disposition = kDispositionState::Create;
          }
        }
        break;
      default:
        assert_unhandled_case(flags_value & 0xF);
        break;
    }

    uint32_t content_license = 0;
    if (disposition == kDispositionState::Create) {
      result = content_manager->CreateContent(root_name, xuid, content_data);
      if (XSUCCEEDED(result)) {
        // The package itself exists and is open, so keep reporting success; a missing header only hides the
        // content from enumeration until it is written again. Log it instead of failing silently.
        const X_RESULT header_result = content_manager->WriteContentHeaderFile(xuid, content_data);
        if (XFAILED(header_result)) {
          REXKRNL_WARN("XamContentCreate: content header could not be written ({:08X})", header_result);
        }
      }
    } else if (disposition == kDispositionState::Open) {
      result = content_manager->OpenContent(root_name, xuid, content_data, content_license);
    }

    if (license_mask_ptr && XSUCCEEDED(result)) {
      *license_mask_ptr = content_license;
    }

    if (disposition_ptr) {
      *disposition_ptr = static_cast<uint32_t>(disposition);
    }

    REXKRNL_INFO("{}: root='{}' content_type={:#x} file_name='{}' flags={:#x} disposition={} "
                 "result={:#x} license={:#x}",
                 api_name, root_name, uint32_t(content_data.content_type.get()),
                 content_data.file_name(),
                 flags_value, static_cast<uint32_t>(disposition), uint32_t(result),
                 content_license);

    extended_error = X_HRESULT_FROM_WIN32(result);
    length = static_cast<uint32_t>(disposition);
    return result;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    return run(extended_error, length);
  } else {
    REX_KERNEL_STATE()->CompleteOverlappedDeferredEx(run, overlapped_ptr.guest_address());
    return X_ERROR_IO_PENDING;
  }
}

ppc_u32_result_t XamContentCreateEx_entry(ppc_u32_t user_index, ppc_pchar_t root_name,
                                          ppc_pvoid_t content_data_ptr, ppc_u32_t flags,
                                          ppc_pu32_t disposition_ptr, ppc_pu32_t license_mask_ptr,
                                          ppc_u32_t cache_size, ppc_u64_t content_size,
                                          ppc_pvoid_t overlapped_ptr) {
  return xeXamContentCreate("XamContentCreateEx", user_index, root_name, content_data_ptr,
                            sizeof(XCONTENT_DATA), flags, disposition_ptr, license_mask_ptr,
                            cache_size, content_size, overlapped_ptr);
}

ppc_u32_result_t XamContentCreate_entry(ppc_u32_t user_index, ppc_pchar_t root_name,
                                        ppc_pvoid_t content_data_ptr, ppc_u32_t flags,
                                        ppc_pu32_t disposition_ptr, ppc_pu32_t license_mask_ptr,
                                        ppc_pvoid_t overlapped_ptr) {
  return xeXamContentCreate("XamContentCreate", user_index, root_name, content_data_ptr,
                            sizeof(XCONTENT_DATA), flags, disposition_ptr, license_mask_ptr, 0, 0,
                            overlapped_ptr);
}

ppc_u32_result_t XamContentCreateInternal_entry(ppc_pchar_t root_name, ppc_pvoid_t content_data_ptr,
                                                ppc_u32_t flags, ppc_pu32_t disposition_ptr,
                                                ppc_pu32_t license_mask_ptr, ppc_u32_t cache_size,
                                                ppc_u64_t content_size,
                                                ppc_pvoid_t overlapped_ptr) {
  return xeXamContentCreate("XamContentCreateInternal", 0xFE, root_name, content_data_ptr,
                            sizeof(XCONTENT_AGGREGATE_DATA), flags, disposition_ptr,
                            license_mask_ptr, cache_size, content_size, overlapped_ptr);
}

ppc_u32_result_t XamContentOpenFile_entry(ppc_u32_t user_index, ppc_pchar_t root_name,
                                          ppc_pchar_t path, ppc_u32_t flags,
                                          ppc_pu32_t disposition_ptr, ppc_pu32_t license_mask_ptr,
                                          ppc_pvoid_t overlapped_ptr) {
  // TODO(gibbed): arguments assumed based on XamContentCreate.
  const auto root = root_name ? root_name.value() : std::string_view("<null>");
  const auto file_path = path ? path.value() : std::string_view("<null>");
  REXKRNL_INFO("XamContentOpenFile: root='{}' path='{}' flags={:#x} result={:#x}",
               root, file_path, uint32_t(flags), uint32_t(X_ERROR_FILE_NOT_FOUND));
  return X_ERROR_FILE_NOT_FOUND;
}

ppc_u32_result_t XamContentFlush_entry(ppc_pchar_t root_name, ppc_pvoid_t overlapped_ptr) {
  X_RESULT result = X_ERROR_SUCCESS;
  const auto root = root_name ? root_name.value() : std::string_view("<null>");
  REXKRNL_INFO("XamContentFlush: root='{}' result={:#x}", root, static_cast<uint32_t>(result));
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(), result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

ppc_u32_result_t XamContentClose_entry(ppc_pchar_t root_name, ppc_pvoid_t overlapped_ptr) {
  // Closes a previously opened root from XamContentCreate*.
  auto result = REX_KERNEL_STATE()->content_manager()->CloseContent(root_name.value());
  REXKRNL_INFO("XamContentClose: root='{}' result={:#x}", root_name.value(),
               static_cast<uint32_t>(result));

  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(), result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

ppc_u32_result_t XamContentGetCreator_entry(ppc_u32_t user_index, ppc_pvoid_t content_data_ptr,
                                            ppc_pu32_t is_creator_ptr, ppc_pu64_t creator_xuid_ptr,
                                            ppc_pvoid_t overlapped_ptr) {
  auto result = X_ERROR_SUCCESS;

  uint64_t xuid = REX_KERNEL_STATE()->user_profile()->xuid();
  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_DATA*>();

  bool content_exists = REX_KERNEL_STATE()->content_manager()->ContentExists(xuid, content_data);

  if (content_exists) {
    if (content_data.content_type == XContentType::kSavedGame) {
      // User always creates saves.
      *is_creator_ptr = 1;
      if (creator_xuid_ptr) {
        *creator_xuid_ptr = xuid;
      }
    } else {
      *is_creator_ptr = 0;
      if (creator_xuid_ptr) {
        *creator_xuid_ptr = 0;
      }
    }
  } else {
    result = X_ERROR_PATH_NOT_FOUND;
  }

  REXKRNL_INFO("XamContentGetCreator: result={:#x}", static_cast<uint32_t>(result));

  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(), result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

ppc_u32_result_t XamContentGetThumbnail_entry(ppc_u32_t user_index, ppc_pvoid_t content_data_ptr,
                                              ppc_pvoid_t buffer_ptr, ppc_pu32_t buffer_size_ptr,
                                              ppc_pvoid_t overlapped_ptr) {
  assert_not_null(buffer_size_ptr);
  uint32_t buffer_size = *buffer_size_ptr;
  uint64_t xuid = REX_KERNEL_STATE()->user_profile()->xuid();
  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_DATA*>();

  // Get thumbnail (if it exists).
  std::vector<uint8_t> buffer;
  auto result =
      REX_KERNEL_STATE()->content_manager()->GetContentThumbnail(xuid, content_data, &buffer);

  *buffer_size_ptr = uint32_t(buffer.size());

  if (XSUCCEEDED(result)) {
    // Write data, if we were given a pointer.
    // This may have just been a size query.
    if (buffer_ptr) {
      if (buffer_size < buffer.size()) {
        // Dest buffer too small.
        result = X_ERROR_INSUFFICIENT_BUFFER;
      } else {
        // Copy data.
        std::memcpy((uint8_t*)buffer_ptr, buffer.data(), buffer.size());
      }
    }
  }

  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(), result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

ppc_u32_result_t XamContentSetThumbnail_entry(ppc_u32_t user_index, ppc_pvoid_t content_data_ptr,
                                              ppc_pvoid_t buffer_ptr, ppc_u32_t buffer_size,
                                              ppc_pvoid_t overlapped_ptr) {
  uint64_t xuid = REX_KERNEL_STATE()->user_profile()->xuid();
  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_DATA*>();

  // Buffer is PNG data.
  auto buffer = std::vector<uint8_t>((uint8_t*)buffer_ptr, (uint8_t*)buffer_ptr + buffer_size);
  auto result = REX_KERNEL_STATE()->content_manager()->SetContentThumbnail(xuid, content_data,
                                                                           std::move(buffer));

  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(), result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

ppc_u32_result_t XamContentDelete_entry(ppc_u32_t user_index, ppc_pvoid_t content_data_ptr,
                                        ppc_pvoid_t overlapped_ptr) {
  uint64_t xuid = REX_KERNEL_STATE()->user_profile()->xuid();
  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_DATA*>();

  auto result = REX_KERNEL_STATE()->content_manager()->DeleteContent(xuid, content_data);

  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(), result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

ppc_u32_result_t XamContentDeleteInternal_entry(ppc_pvoid_t content_data_ptr,
                                                ppc_pvoid_t overlapped_ptr) {
  // INFO: Analysis of xam.xex shows that "internal" functions are wrappers with
  // 0xFE as user_index
  uint64_t xuid = REX_KERNEL_STATE()->user_profile()->xuid();
  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_AGGREGATE_DATA*>();

  auto result = REX_KERNEL_STATE()->content_manager()->DeleteContent(xuid, content_data);

  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(), result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

XAM_EXPORT(__imp__XamContentGetLicenseMask, rex::kernel::xam::XamContentGetLicenseMask_entry)
XAM_EXPORT(__imp__XamContentResolve, rex::kernel::xam::XamContentResolve_entry)
XAM_EXPORT(__imp__XamContentCreateEnumerator, rex::kernel::xam::XamContentCreateEnumerator_entry)
XAM_EXPORT(__imp__XamContentCreateEx, rex::kernel::xam::XamContentCreateEx_entry)
XAM_EXPORT(__imp__XamContentCreate, rex::kernel::xam::XamContentCreate_entry)
XAM_EXPORT(__imp__XamContentCreateInternal, rex::kernel::xam::XamContentCreateInternal_entry)
XAM_EXPORT(__imp__XamContentOpenFile, rex::kernel::xam::XamContentOpenFile_entry)
XAM_EXPORT(__imp__XamContentFlush, rex::kernel::xam::XamContentFlush_entry)
XAM_EXPORT(__imp__XamContentClose, rex::kernel::xam::XamContentClose_entry)
XAM_EXPORT(__imp__XamContentGetCreator, rex::kernel::xam::XamContentGetCreator_entry)
XAM_EXPORT(__imp__XamContentGetThumbnail, rex::kernel::xam::XamContentGetThumbnail_entry)
XAM_EXPORT(__imp__XamContentSetThumbnail, rex::kernel::xam::XamContentSetThumbnail_entry)
XAM_EXPORT(__imp__XamContentDelete, rex::kernel::xam::XamContentDelete_entry)
XAM_EXPORT(__imp__XamContentDeleteInternal, rex::kernel::xam::XamContentDeleteInternal_entry)

#define XAM_CONTENT_EXPORT_STUB(name, export_name)                                \
  extern "C" PPC_WEAK_FUNC(name) {                                                \
    (void)base;                                                                   \
    REXKRNL_INFO("{} called", export_name);                                      \
    REXKRNL_WARN("{} STUB", #name);                                              \
  }                                                                               \
  static rex::detail::PPCFuncRegistrar _ppc_reg_##name(#name, &name);

XAM_CONTENT_EXPORT_STUB(__imp__XamContentClosePackageFile, "XamContentClosePackageFile")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentCopyInternal, "XamContentCopyInternal")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentCreateAndMountPackage, "XamContentCreateAndMountPackage")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentCreateEnumeratorInternal,
                        "XamContentCreateEnumeratorInternal")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentDismountAndClosePackage,
                        "XamContentDismountAndClosePackage")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentDuplicateFileHandle, "XamContentDuplicateFileHandle")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentExistsOnDeviceInternal,
                        "XamContentExistsOnDeviceInternal")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentFlushPackage, "XamContentFlushPackage")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentGetAttributes, "XamContentGetAttributes")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentGetAttributesInternal,
                        "XamContentGetAttributesInternal")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentGetHeaderInternal, "XamContentGetHeaderInternal")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentGetLocalizedString, "XamContentGetLocalizedString")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentGetMetaDataInternal, "XamContentGetMetaDataInternal")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentGetMountedPackageByRootName,
                        "XamContentGetMountedPackageByRootName")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentGetOnlineCreator, "XamContentGetOnlineCreator")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentInstall, "XamContentInstall")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentInstallInternal, "XamContentInstallInternal")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentIsGameInstalledToHDD, "XamContentIsGameInstalledToHDD")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentLaunchImage, "XamContentLaunchImage")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentLaunchImageFromFileInternal,
                        "XamContentLaunchImageFromFileInternal")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentLaunchImageInternal, "XamContentLaunchImageInternal")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentLaunchImageInternalEx,
                        "XamContentLaunchImageInternalEx")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentLockUnlockPackageHeaders,
                        "XamContentLockUnlockPackageHeaders")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentMountInstalledGame, "XamContentMountInstalledGame")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentMountPackage, "XamContentMountPackage")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentMoveInternal, "XamContentMoveInternal")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentOpenFileInternal, "XamContentOpenFileInternal")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentOpenPackageFile, "XamContentOpenPackageFile")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentQueryLicenseInternal, "XamContentQueryLicenseInternal")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentRegisterChangeCallback,
                        "XamContentRegisterChangeCallback")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentResolveInternal, "XamContentResolveInternal")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentSetAttributes, "XamContentSetAttributes")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentSetMediaMetaDataInternal,
                        "XamContentSetMediaMetaDataInternal")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentSetThumbnailInternal, "XamContentSetThumbnailInternal")
XAM_CONTENT_EXPORT_STUB(__imp__XamContentWritePackageHeader, "XamContentWritePackageHeader")

#undef XAM_CONTENT_EXPORT_STUB
