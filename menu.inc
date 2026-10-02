<?php

# One entry per menu area (PLUGIN_GUIDELINES.md 9.1).
$menuEntries = Array(
    Array(
        'text' => 'Listen Sync',
        'type' => 'status',
        'page' => 'status.php',
        'wrap' => 1
    ),
    Array(
        'text' => 'Listen Sync - About',
        'type' => 'help',
        'page' => 'about.php',
        'wrap' => 1
    )
);

foreach ($menuEntries as $entry)
{
    if ($entry['type'] != $menu)
    {
        continue;
    }

    $nopage = '';

    if (isset($entry['wrap']) && ($entry['wrap'] == 0))
    {
        $nopage = '&nopage=1';
    }

    printf("<li><a href='plugin.php?plugin=%s&page=%s%s'>%s</a></li>\n",
        $plugin, $entry['page'], $nopage, $entry['text']);
}
?>
