compat_sources = files('compat.c')
compat_symbols = []
subdir('rockchip-rga')
subdir('rockchip-hantro')
compat_declarations = ''
compat_entries = ''
foreach symbol : compat_symbols
  compat_declarations += 'extern const struct vareq_compat_module ' + symbol + ';\n'
  compat_entries += '    &' + symbol + ',\n'
endforeach
compat_config = configuration_data()
compat_config.set('COMPAT_DECLARATIONS', compat_declarations)
compat_config.set('COMPAT_ENTRIES', compat_entries)
compat_sources += configure_file(input: 'registry.c.in', output: 'registry.c', configuration: compat_config)
