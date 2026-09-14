unit = executable('test-util', 'test-util.c', include_directories: inc,
  link_with: core, dependencies: gio)
test('input-validation', unit)
integration = executable('test-bluez', 'test-bluez.c', include_directories: inc,
  link_with: core, dependencies: gio)
test('bluez-integration', integration, timeout: 60)
