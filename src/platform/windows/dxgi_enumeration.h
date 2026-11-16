/**
 * @file src/platform/windows/dxgi_enumeration.h
 * @brief DXGI adapter and output enumeration that stops on any failure.
 */
#pragma once

// platform includes
#include <dxgi.h>

namespace platf::dxgi {
  /**
   * @brief Call a function for each adapter of a DXGI factory.
   *
   * DXGI ends the enumeration with `DXGI_ERROR_NOT_FOUND`, but it can also fail with other errors, for example in
   * a session without a desktop, and returns no adapter then.
   *
   * @tparam Adapter Owning pointer type the adapters are passed in.
   * @param factory Factory whose adapters to enumerate.
   * @param callback Called with each adapter; returns `false` to stop the enumeration.
   * @return `S_OK` after the last adapter or when the callback stops, otherwise the `EnumAdapters1()` failure.
   */
  template<class Adapter, class Callback>
  HRESULT for_each_adapter(IDXGIFactory1 *factory, Callback &&callback) {
    for (UINT index = 0;; ++index) {
      IDXGIAdapter1 *adapter_p = nullptr;
      if (const HRESULT status = factory->EnumAdapters1(index, &adapter_p); FAILED(status)) {
        return status == DXGI_ERROR_NOT_FOUND ? S_OK : status;
      }

      if (Adapter adapter {adapter_p}; !callback(adapter)) {
        return S_OK;
      }
    }
  }

  /**
   * @brief Call a function for each output of a DXGI adapter.
   *
   * DXGI ends the enumeration with `DXGI_ERROR_NOT_FOUND`, but it can also fail with other errors, for example in
   * a session without a desktop, and returns no output then.
   *
   * @tparam Output Owning pointer type the outputs are passed in.
   * @param adapter Adapter whose outputs to enumerate.
   * @param callback Called with each output; returns `false` to stop the enumeration.
   * @return `S_OK` after the last output or when the callback stops, otherwise the `EnumOutputs()` failure.
   */
  template<class Output, class Callback>
  HRESULT for_each_output(IDXGIAdapter *adapter, Callback &&callback) {
    for (UINT index = 0;; ++index) {
      IDXGIOutput *output_p = nullptr;
      if (const HRESULT status = adapter->EnumOutputs(index, &output_p); FAILED(status)) {
        return status == DXGI_ERROR_NOT_FOUND ? S_OK : status;
      }

      if (Output output {output_p}; !callback(output)) {
        return S_OK;
      }
    }
  }
}  // namespace platf::dxgi
