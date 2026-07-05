import { useEffect, useState, type FormEvent } from "react";

interface Task {
  id: number;
  title: string;
  done: boolean;
}

export default function App() {
  const [tasks, setTasks] = useState<Task[]>([]);
  const [title, setTitle] = useState("");
  const [error, setError] = useState<string | null>(null);

  async function load() {
    try {
      const response = await fetch("/api/tasks");
      const data: { tasks: Task[] } = await response.json();
      setTasks(data.tasks);
      setError(null);
    } catch {
      setError("could not reach the prism API");
    }
  }

  useEffect(() => {
    void load();
  }, []);

  async function add(event: FormEvent) {
    event.preventDefault();
    if (!title.trim()) return;
    const response = await fetch("/api/tasks", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ title }),
    });
    if (response.ok) {
      setTitle("");
      void load();
    }
  }

  async function toggle(id: number) {
    await fetch(`/api/tasks/${id}`, { method: "PUT" });
    void load();
  }

  return (
    <main>
      <h1>prism + react</h1>
      <p>
        This React app is served by prism, and the task list below comes from
        prism's REST API on the same server.
      </p>
      {error && <p className="error">{error}</p>}
      <form onSubmit={add}>
        <input
          value={title}
          onChange={(event) => setTitle(event.target.value)}
          placeholder="new task"
          aria-label="new task"
        />
        <button type="submit">Add</button>
      </form>
      <ul>
        {tasks.map((task) => (
          <li
            key={task.id}
            className={task.done ? "done" : ""}
            onClick={() => void toggle(task.id)}
          >
            <span className="check">{task.done ? "✓" : "○"}</span>
            {task.title}
          </li>
        ))}
      </ul>
      <p className="hint">Click a task to toggle it.</p>
    </main>
  );
}
