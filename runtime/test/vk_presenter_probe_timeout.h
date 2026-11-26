// vk_presenter_sync_test --probe-timeout (RT-PRESENT-PROBE-TIMEOUT-UAF): the present-source readback probe
// (LEONIDA_CAPTURE_SOURCE) gives up when its fence wait fails, but the copy it submitted may still be running. The
// driver hooks in vk_presenter_sync_test.cpp hold that copy on a timeline semaphore and make the wait time out at
// once. The probe must not free the staging buffer or its memory while the copy is pending, must not reset and
// resubmit its command buffer and fence for a later present while they are in use (that probe is skipped), must take
// readbacks again once the copy is done, and must leave nothing behind at vkDestroyDevice (run it with
// LEONIDA_VK_VALIDATION=1 for the layer's own checks). Included after the driver hooks. No game data.
#pragma once

namespace probe_timeout_test {
namespace fs = std::filesystem;

// A fresh owner/SYSTEM-only capture folder; only the files the probe and the capture write in it are removed.
struct Folder {
    fs::path path;
    bool create() {
        auto candidate = fs::temp_directory_path() / ("leonida-probe-timeout-" + std::to_string(GetCurrentProcessId()) +
                                                      "-" + std::to_string(GetTickCount64()));
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)",
                                                                  SDDL_REVISION_1, &sd, nullptr)) return false;
        SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
        const bool fresh = CreateDirectoryW(candidate.c_str(), &sa) != FALSE;
        LocalFree(sd);
        if (fresh) path = std::move(candidate);
        return fresh;
    }
    ~Folder() {
        if (path.empty()) return;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(path, ec)) {
            const std::string name = e.path().filename().string();
            const bool ours = name == "index.txt" || name == "source_index.txt" ||
                              ((name.rfind("frame_", 0) == 0 || name.rfind("source_", 0) == 0) &&
                               e.path().extension() == ".ppm");
            if (ours && fs::is_regular_file(e.symlink_status())) fs::remove(e.path(), ec);
        }
        fs::remove(path, ec);  // leave unknown contents alone
    }
};

inline int run() {
    using namespace leonida;
    Folder folder;
    if (!folder.create()) {
        std::fprintf(stderr, "probe timeout: cannot make a capture folder\n");
        return 1;
    }
    _putenv_s("LEONIDA_CAPTURE", folder.path.string().c_str());
    _putenv_s("LEONIDA_CAPTURE_SOURCE", "1");
    _putenv_s("LEONIDA_CAPTURE_EVERY", "1");

    auto d = std::make_unique<vktest::Device>();
    if (!d->create(true)) {
        std::fprintf(stderr, "vk_presenter_sync_test --probe-timeout: no Vulkan device with presentation, skipped\n");
        return 77;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"LeonidaPresenterProbeTimeoutTest";
    EXPECT(RegisterClassW(&wc));
    constexpr std::uint32_t width = 64, height = 48;
    RECT fr{0, 0, LONG(width), LONG(height)};
    AdjustWindowRect(&fr, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"Leonida probe timeout test",
                                  WS_OVERLAPPEDWINDOW, 80, 80, fr.right - fr.left, fr.bottom - fr.top, nullptr, nullptr,
                                  wc.hInstance, nullptr);
    EXPECT(window);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    pump();
    driver::install(d->instance_proc, d->instance, d->device);

    const auto create_semaphore =
        reinterpret_cast<PFN_vkCreateSemaphore>(driver::real_get_device(d->device, "vkCreateSemaphore"));
    const auto destroy_semaphore =
        reinterpret_cast<PFN_vkDestroySemaphore>(driver::real_get_device(d->device, "vkDestroySemaphore"));
    const auto signal_semaphore =
        reinterpret_cast<PFN_vkSignalSemaphore>(driver::real_get_device(d->device, "vkSignalSemaphore"));
    EXPECT(create_semaphore && destroy_semaphore && signal_semaphore);
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
    VkSemaphore hold = VK_NULL_HANDLE;
    EXPECT(create_semaphore(d->device, &semaphore_info, nullptr, &hold) == VK_SUCCESS);

    bool first_read = false, kept = false, one_submit_while_held = false, read_again = false, idle = false;
    VkResult presents[4] = {VK_ERROR_UNKNOWN, VK_ERROR_UNKNOWN, VK_ERROR_UNKNOWN, VK_ERROR_UNKNOWN};
    {
        VulkanContext context(driver::get_device, d->device, true);
        EXPECT(context.loaded());
        VulkanQueue queue(context, d->queue, d->family);
        EXPECT(queue.ok());
        const VkImage image = d->image(VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 1,
                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        EXPECT(image != VK_NULL_HANDLE);
        ResourceBinding rb{};
        rb.image = image;
        rb.format = VK_FORMAT_R8G8B8A8_UNORM;
        rb.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
        rb.width = 32;
        rb.height = 32;
        rb.mip_levels = 1;
        rb.array_layers = 1;
        rb.depth = 1;
        rb.samples = 1;
        rb.color_attachment = true;
        context.add_resource(1, rb, VK_IMAGE_LAYOUT_UNDEFINED);
        {  // a written source: the probe only reads back what a tracked GPU operation wrote
            CommandList clear;
            EXPECT(clear.transition({1, 0, d3d12_state::common, d3d12_state::render_target}));
            EXPECT(clear.clear_render_target({1, 0, 0, 1}, {0.2f, 0.6f, 0.9f, 1.0f}));
            EXPECT(clear.transition({1, 0, d3d12_state::render_target, d3d12_state::common}));
            EXPECT(clear.close());
            const CommandList* ptr = &clear;
            EXPECT(queue.execute({&ptr, 1}) == ReplayResult::success);
            EXPECT(queue.idle());
        }
        driver::probe_image = image;

        const auto create_surface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
            d->instance_proc(d->instance, "vkCreateWin32SurfaceKHR"));
        EXPECT(create_surface);
        VkWin32SurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surface_info.hinstance = GetModuleHandleW(nullptr);
        surface_info.hwnd = window;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        EXPECT(create_surface(d->instance, &surface_info, nullptr, &surface) == VK_SUCCESS);
        {
            VulkanPresenter presenter(context, driver::get_instance, d->instance, d->physical, d->queue, d->family,
                                      surface);
            EXPECT(presenter.ok());

            // 1. A plain readback: the probe is on and copies the source.
            presents[0] = presenter.present(1, true);
            first_read = driver::probe_submits.load() == 1;

            // 2. The next readback's copy is held and its wait times out. The frame's own submit is made to fail
            //    (nothing may be presented behind the held copy), so this present reports that failure. No EXPECT
            //    may return from here until the semaphore is signalled: the destructor would wait for the queue.
            driver::hold = hold;
            driver::held.store(true);
            presents[1] = presenter.present(1, true);
            kept = !driver::freed_in_flight.load() && driver::held_buffer != VK_NULL_HANDLE;

            // 3. A present while the copy is still held: its probe, which runs first, must not reset the busy
            //    command buffer and fence and submit again. The rest of that present waits for the queue (the image
            //    the failed present gave back is fenced behind the held copy) until the release thread signals.
            VkResult signalled = VK_ERROR_UNKNOWN;
            std::thread release([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                driver::held.store(false);
                VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
                signal.semaphore = hold;
                signal.value = 1;
                signalled = signal_semaphore(d->device, &signal);
            });
            presents[2] = presenter.present(1, true);
            release.join();
            one_submit_while_held = driver::probe_submits_held.load() == 1 && driver::probe_submits.load() == 2 &&
                                    !driver::freed_in_flight.load() && signalled == VK_SUCCESS;

            // 4. With the copy done, the parked buffer is freed and the probe reads back again.
            presents[3] = presenter.present(1, true);
            read_again = driver::probe_submits.load() == 3;
            idle = queue.idle();
            driver::probe_image = VK_NULL_HANDLE;
        }
        pump();
        context.remove_resource(1);
    }
    destroy_semaphore(d->device, hold, nullptr);
    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    d.reset();  // vkDestroyDevice: the layer reports any object still alive here
    for (int i = 0; i < 4; ++i) std::fprintf(stderr, "probe timeout: present %d returned %d\n", i, int(presents[i]));
    EXPECT(presents[0] == VK_SUCCESS && presents[1] == VK_ERROR_OUT_OF_HOST_MEMORY &&
           presents[2] == VK_SUCCESS && presents[3] == VK_SUCCESS);
    EXPECT(first_read);
    EXPECT(kept);
    EXPECT(one_submit_while_held);
    EXPECT(read_again);
    EXPECT(idle);
    EXPECT(vktest::validation_error_count() == 0);
    std::printf("vk_presenter_sync_test --probe-timeout: ok\n");
    return 0;
}

}  // namespace probe_timeout_test
