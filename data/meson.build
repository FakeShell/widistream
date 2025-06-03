subdir('icons')

desktop_file = i18n.merge_file(
  input: 'io.furios.WiDiStream.desktop.in',
  output: 'io.furios.WiDiStream.desktop',
  type: 'desktop',
  po_dir: '../po',
  install: true,
  install_dir: join_paths(get_option('datadir'), 'applications')
)

desktop_utils = find_program('desktop-file-validate', required: false)
if desktop_utils.found()
  test('Validate desktop file', desktop_utils,
    args: [desktop_file]
  )
endif

if enable_firewalld_zone
  install_data(
    'P2P-WiFi-Display.xml',
    install_dir: join_paths(get_option('prefix'), 'lib', 'firewalld', 'zones')
  )
endif
