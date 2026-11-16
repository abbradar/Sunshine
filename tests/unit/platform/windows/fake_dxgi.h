/**
 * @file tests/unit/platform/windows/fake_dxgi.h
 * @brief DXGI factory, adapter and output substitutes for enumeration tests.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

// platform includes
#include <dxgi.h>

namespace fake_dxgi {
  /**
   * @brief Common COM and DXGI object methods of the substitutes.
   *
   * The objects live in the test scope; reference counting only tracks whether enumeration released them.
   */
  template<class Interface>
  class object_t: public Interface {
  public:
    ULONG references = 1;  ///< References held, starting with the test's own.

    /**
     * @brief Reject interface queries.
     */
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **object) override {  // NOSONAR(cpp:S5008): Windows COM signature
      *object = nullptr;
      return E_NOINTERFACE;
    }

    /**
     * @brief Count a new reference.
     */
    ULONG STDMETHODCALLTYPE AddRef() override {
      return ++references;
    }

    /**
     * @brief Count a released reference without destroying the test-owned object.
     */
    ULONG STDMETHODCALLTYPE Release() override {
      return --references;
    }

    /**
     * @brief Reject private data writes.
     */
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override {
      return E_NOTIMPL;
    }

    /**
     * @brief Reject private interface writes.
     */
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override {
      return E_NOTIMPL;
    }

    /**
     * @brief Reject private data reads.
     */
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override {
      return E_NOTIMPL;
    }

    /**
     * @brief Reject parent queries.
     */
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void **parent) override {  // NOSONAR(cpp:S5008): Windows COM signature
      *parent = nullptr;
      return E_NOTIMPL;
    }
  };

  /**
   * @brief Return the object at an enumeration index, or end the enumeration with a configured status.
   *
   * @param objects Objects to enumerate.
   * @param end_status Status returned after the last object.
   * @param index Requested index.
   * @param object Receives a new reference to the object, or `nullptr`.
   * @return `S_OK`, or `end_status` past the last object.
   */
  template<class Object, class Interface>
  HRESULT enumerate(const std::vector<Object *> &objects, HRESULT end_status, UINT index, Interface **object) {
    if (index >= objects.size()) {
      *object = nullptr;
      return end_status;
    }
    objects[index]->AddRef();
    *object = objects[index];
    return S_OK;
  }

  /**
   * @brief Output with a configurable descriptor.
   */
  class output_t final: public object_t<IDXGIOutput> {
  public:
    DXGI_OUTPUT_DESC desc {};  ///< Descriptor returned by GetDesc().

    /**
     * @brief Create an output.
     *
     * @param name Device name, such as `\\.\DISPLAY1`.
     * @param attached Whether the output is part of the desktop.
     */
    explicit output_t(std::wstring_view name, bool attached = true) {
      std::ranges::copy(name.substr(0, std::size(desc.DeviceName) - 1), desc.DeviceName);
      desc.AttachedToDesktop = attached;
      desc.DesktopCoordinates = {0, 0, 1920, 1080};
      desc.Rotation = DXGI_MODE_ROTATION_IDENTITY;
    }

    /**
     * @brief Copy the configured descriptor.
     */
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_OUTPUT_DESC *out) override {
      *out = desc;
      return S_OK;
    }

    /**
     * @brief Reject display mode queries.
     */
    HRESULT STDMETHODCALLTYPE GetDisplayModeList(DXGI_FORMAT, UINT, UINT *, DXGI_MODE_DESC *) override {
      return E_NOTIMPL;
    }

    /**
     * @brief Reject display mode matching.
     */
    HRESULT STDMETHODCALLTYPE FindClosestMatchingMode(const DXGI_MODE_DESC *, DXGI_MODE_DESC *, IUnknown *) override {
      return E_NOTIMPL;
    }

    /**
     * @brief Reject vertical blank waits.
     */
    HRESULT STDMETHODCALLTYPE WaitForVBlank() override {
      return E_NOTIMPL;
    }

    /**
     * @brief Reject exclusive ownership.
     */
    HRESULT STDMETHODCALLTYPE TakeOwnership(IUnknown *, BOOL) override {
      return E_NOTIMPL;
    }

    /**
     * @brief Ignore ownership release.
     */
    void STDMETHODCALLTYPE ReleaseOwnership() override {
      // No ownership is ever taken
    }

    /**
     * @brief Reject gamma capability queries.
     */
    HRESULT STDMETHODCALLTYPE GetGammaControlCapabilities(DXGI_GAMMA_CONTROL_CAPABILITIES *) override {
      return E_NOTIMPL;
    }

    /**
     * @brief Reject gamma writes.
     */
    HRESULT STDMETHODCALLTYPE SetGammaControl(const DXGI_GAMMA_CONTROL *) override {
      return E_NOTIMPL;
    }

    /**
     * @brief Reject gamma reads.
     */
    HRESULT STDMETHODCALLTYPE GetGammaControl(DXGI_GAMMA_CONTROL *) override {
      return E_NOTIMPL;
    }

    /**
     * @brief Reject display surface writes.
     */
    HRESULT STDMETHODCALLTYPE SetDisplaySurface(IDXGISurface *) override {
      return E_NOTIMPL;
    }

    /**
     * @brief Reject display surface reads.
     */
    HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData(IDXGISurface *) override {
      return E_NOTIMPL;
    }

    /**
     * @brief Reject frame statistics queries.
     */
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS *) override {
      return E_NOTIMPL;
    }
  };

  /**
   * @brief Adapter with configurable outputs.
   */
  class adapter_t final: public object_t<IDXGIAdapter1> {
  public:
    DXGI_ADAPTER_DESC1 desc {};  ///< Descriptor returned by GetDesc1().
    std::vector<output_t *> outputs;  ///< Outputs returned by EnumOutputs().
    HRESULT outputs_end = DXGI_ERROR_NOT_FOUND;  ///< Status returned after the last output.
    std::size_t enum_outputs_calls = 0;  ///< Number of EnumOutputs() calls.

    /**
     * @brief Create an adapter.
     *
     * @param name Adapter description, such as `NVIDIA Tesla T4`.
     * @param outputs Outputs of the adapter.
     */
    explicit adapter_t(std::wstring_view name, std::vector<output_t *> outputs = {}):
        outputs {std::move(outputs)} {
      std::ranges::copy(name.substr(0, std::size(desc.Description) - 1), desc.Description);
    }

    /**
     * @brief Return the configured outputs.
     */
    HRESULT STDMETHODCALLTYPE EnumOutputs(UINT index, IDXGIOutput **output) override {
      ++enum_outputs_calls;
      return enumerate(outputs, outputs_end, index, output);
    }

    /**
     * @brief Copy the configured descriptor without the DXGI 1.1 flags.
     */
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC *out) override {
      std::ranges::copy(desc.Description, out->Description);
      out->VendorId = desc.VendorId;
      out->DeviceId = desc.DeviceId;
      out->SubSysId = desc.SubSysId;
      out->Revision = desc.Revision;
      out->DedicatedVideoMemory = desc.DedicatedVideoMemory;
      out->DedicatedSystemMemory = desc.DedicatedSystemMemory;
      out->SharedSystemMemory = desc.SharedSystemMemory;
      out->AdapterLuid = desc.AdapterLuid;
      return S_OK;
    }

    /**
     * @brief Reject interface support queries.
     */
    HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID, LARGE_INTEGER *) override {
      return E_NOTIMPL;
    }

    /**
     * @brief Copy the configured descriptor.
     */
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1 *out) override {
      *out = desc;
      return S_OK;
    }
  };

  /**
   * @brief Factory with configurable adapters.
   */
  class factory_t final: public object_t<IDXGIFactory1> {
  public:
    std::vector<adapter_t *> adapters;  ///< Adapters returned by EnumAdapters1().
    HRESULT adapters_end = DXGI_ERROR_NOT_FOUND;  ///< Status returned after the last adapter.

    /**
     * @brief Create a factory.
     *
     * @param adapters Adapters of the factory.
     */
    explicit factory_t(std::vector<adapter_t *> adapters = {}):
        adapters {std::move(adapters)} {
    }

    /**
     * @brief Reject DXGI 1.0 adapter enumeration.
     */
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT, IDXGIAdapter **adapter) override {
      *adapter = nullptr;
      return E_NOTIMPL;
    }

    /**
     * @brief Reject window association.
     */
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND, UINT) override {
      return E_NOTIMPL;
    }

    /**
     * @brief Reject window association queries.
     */
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND *) override {
      return E_NOTIMPL;
    }

    /**
     * @brief Reject swap chain creation.
     */
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **) override {
      return E_NOTIMPL;
    }

    /**
     * @brief Reject software adapter creation.
     */
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE, IDXGIAdapter **) override {
      return E_NOTIMPL;
    }

    /**
     * @brief Return the configured adapters.
     */
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT index, IDXGIAdapter1 **adapter) override {
      return enumerate(adapters, adapters_end, index, adapter);
    }

    /**
     * @brief Report the adapter list as current.
     */
    BOOL STDMETHODCALLTYPE IsCurrent() override {
      return TRUE;
    }
  };
}  // namespace fake_dxgi
