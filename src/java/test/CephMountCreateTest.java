/*
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
import java.io.FileNotFoundException;
import org.junit.*;
import static org.junit.Assert.*;

import com.ceph.fs.*;

/*
 * This tests the mount root dir functionality. It creates an empty
 * directory in the real root, then it re-mounts the file system
 * with the empty directory specified as the root. Assertions are
 * that the "/" in the normal mount is non-empty, and that "/" is
 * empty in the mount with the empty directory as the root.
 */
public class CephMountCreateTest {

  private static String conf_file;

  @BeforeClass
  public static void class_setup() throws Exception {
    conf_file = System.getProperty("CEPH_CONF_FILE");
  }

  private CephMount setupMount(String root) throws Exception {
    CephMount mount = new CephMount("admin");
    if (conf_file != null)
      mount.conf_read_file(conf_file);
    mount.mount(root);
    return mount;
  }

  @Test
  public void test_CephMountCreate() throws Exception {
    CephMount mount;
    boolean found;

    /* root dir has more than one dir */
    mount = setupMount(null);

    try {
      mount.rmdir("/libcephfs_java_test_dir");
    } catch (FileNotFoundException e) {}
    mount.mkdirs("/libcephfs_java_test_dir", 777);
    String[] subdirs = mount.listdir("/");
    found = false;
    for (String d : subdirs) {
      if (d.compareTo("libcephfs_java_test_dir") == 0)
        found = true;
    }
    assertTrue(found);
    mount.shutdown();

    /* changing root to empty dir */
    mount = setupMount("/libcephfs_java_test_dir");

    subdirs = mount.listdir("/");
    found = false;
    for (String d : subdirs) {
      if (d.compareTo(".") != 0 && d.compareTo("..") != 0)
        found = true;
    }
    assertFalse(found);
    mount.shutdown();

    /* cleanup */
    mount = setupMount(null);
    mount.rmdir("/libcephfs_java_test_dir");
    mount.shutdown();
  }
}
