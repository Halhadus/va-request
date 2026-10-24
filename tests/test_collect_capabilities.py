import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("caps", Path(__file__).parents[1] / "tools/collect_capabilities.py")
caps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(caps)


class Inventory(unittest.TestCase):
    def test_only_existing_paths_and_read_queries(self):
        calls = []
        def query(executable, node, option):
            calls.append((node, option))
            formats = {"/dev/video1": "'S264'", "/dev/video2": "'AV1F'", "/dev/video3": "'VP9F'",
                       "/dev/video4": "'NV12'", "/dev/video5": "'NV12'"}
            text = formats[node] if "out" in option else "'JPEG'" if node.endswith("4") else "'NV12'"
            return {"option": option, "exit": 0, "stdout": text}
        with patch.object(caps, "query", side_effect=query):
            result = caps.collect("tool", [f"/dev/video{i}" for i in range(1, 6)])
        self.assertTrue(result["complete"])
        self.assertEqual([n["roles"] for n in result["nodes"]], [["H264"], ["AV1"], ["VP9"], ["JPEG encode"], ["raw M2M candidate"]])
        for node in ("/dev/video2", "/dev/video3"):
            self.assertIn("--list-ctrls-menus", [option for n, option in calls if n == node])
        self.assertFalse(any("set-" in option or "stream" in option for _, option in calls))

    def test_failed_selected_query_is_incomplete_and_persisted(self):
        def query(executable, node, option):
            return {"option": option, "exit": 1 if option == "--list-ctrls-menus" else 0, "stdout": "'S265'"}
        with patch.object(caps, "query", side_effect=query): result = caps.collect("tool", ["/dev/video1"])
        self.assertFalse(result["complete"])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "report.json"
            caps.write_report(result, path)
            self.assertEqual(json.loads(path.read_text()), result)
            self.assertEqual(list(Path(directory).iterdir()), [path])

    def test_rgb_only_raw_node_is_inventoried(self):
        def query(executable, node, option):
            return {"option": option, "exit": 0, "stdout": "'RGBP' 'AB24'"}
        with patch.object(caps, "query", side_effect=query): result = caps.collect("tool", ["/dev/video512"])
        self.assertEqual(result["nodes"][0]["roles"], ["raw M2M candidate"])
        self.assertTrue(result["complete"])

    def test_stateful_nodes_are_codecs_not_raw_processors(self):
        def query(executable, node, option):
            return {"option": option, "exit": 0, "stdout": "'H264' 'HEVC'" if 'out' in option else "'NV12'"}
        with patch.object(caps, "query", side_effect=query): result = caps.collect("tool", ["/dev/video2048"])
        self.assertEqual(result['nodes'][0]['roles'], ['H264', 'HEVC'])
        self.assertNotIn('raw M2M candidate', result['nodes'][0]['roles'])
        self.assertEqual([entry['api'] for entry in result['nodes'][0]['decoder_interfaces']], ['stateful', 'stateful'])

    def test_empty_inventory_is_not_success(self):
        self.assertFalse(caps.collect("tool", [])["complete"])


if __name__ == "__main__":
    unittest.main()
